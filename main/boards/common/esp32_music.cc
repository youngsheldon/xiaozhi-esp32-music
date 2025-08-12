#include "esp32_music.h"
#include "board.h"
#include "system_info.h"
#include "audio_codecs/audio_codec.h"
#include "application.h"
#include "protocols/protocol.h"
#include "display/display.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <cJSON.h>
#include <cstring>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype> // 为isdigit函数
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "KwWork.h"

#define TAG "Esp32Music"

// URL编码函数
static std::string url_encode(const std::string &str)
{
    std::string encoded;
    char hex[4];

    for (size_t i = 0; i < str.length(); i++)
    {
        unsigned char c = str[i];

        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            encoded += c;
        }
        else if (c == ' ')
        {
            encoded += '+'; // 空格编码为'+'或'%20'
        }
        else
        {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

// 在文件开头添加一个辅助函数，统一处理URL构建
static std::string buildUrlWithParams(const std::string &base_url, const std::string &path, const std::string &query)
{
    std::string result_url = base_url + path + "?";
    size_t pos = 0;
    size_t amp_pos = 0;

    while ((amp_pos = query.find("&", pos)) != std::string::npos)
    {
        std::string param = query.substr(pos, amp_pos - pos);
        size_t eq_pos = param.find("=");

        if (eq_pos != std::string::npos)
        {
            std::string key = param.substr(0, eq_pos);
            std::string value = param.substr(eq_pos + 1);
            result_url += key + "=" + url_encode(value) + "&";
        }
        else
        {
            result_url += param + "&";
        }

        pos = amp_pos + 1;
    }

    // 处理最后一个参数
    std::string last_param = query.substr(pos);
    size_t eq_pos = last_param.find("=");

    if (eq_pos != std::string::npos)
    {
        std::string key = last_param.substr(0, eq_pos);
        std::string value = last_param.substr(eq_pos + 1);
        result_url += key + "=" + url_encode(value);
    }
    else
    {
        result_url += last_param;
    }

    return result_url;
}

Esp32Music::Esp32Music() : last_downloaded_data_(), current_music_url_(), current_song_name_(),
                           song_name_displayed_(false), current_lyric_url_(), lyrics_(),
                           current_lyric_index_(-1), lyric_thread_(), is_lyric_running_(false),
                           is_playing_(false), is_downloading_(false),
                           play_thread_(), download_thread_(), play_next_(), songs_played_(), audio_buffer_(), buffer_mutex_(),
                           buffer_cv_(), play_next_cv_(), next_mutex_(), need_to_play_next_(false), buffer_size_(0), mp3_decoder_(nullptr), mp3_frame_info_(),
                           mp3_decoder_initialized_(false)
{
    ESP_LOGI(TAG, "Music player initialized");
    InitializeMp3Decoder();
    std::thread(&Esp32Music::PlayNextDetect, this).detach();
}

Esp32Music::~Esp32Music()
{
    ESP_LOGI(TAG, "Destroying music player - stopping all operations");

    // 停止所有操作
    is_downloading_ = false;
    is_playing_ = false;
    is_lyric_running_ = false;

    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    // 等待下载线程结束，设置5秒超时
    if (download_thread_.joinable())
    {
        ESP_LOGI(TAG, "Waiting for download thread to finish (timeout: 5s)");
        auto start_time = std::chrono::steady_clock::now();

        // 等待线程结束
        bool thread_finished = false;
        while (!thread_finished)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - start_time)
                               .count();

            if (elapsed >= 5)
            {
                ESP_LOGW(TAG, "Download thread join timeout after 5 seconds");
                break;
            }

            // 再次设置停止标志，确保线程能够检测到
            is_downloading_ = false;

            // 通知条件变量
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }

            // 检查线程是否已经结束
            if (!download_thread_.joinable())
            {
                thread_finished = true;
            }

            // 定期打印等待信息
            if (elapsed > 0 && elapsed % 1 == 0)
            {
                ESP_LOGI(TAG, "Still waiting for download thread to finish... (%ds)", (int)elapsed);
            }
        }

        if (download_thread_.joinable())
        {
            download_thread_.join();
        }
        ESP_LOGI(TAG, "Download thread finished");
    }

    // 等待播放线程结束，设置3秒超时
    if (play_thread_.joinable())
    {
        ESP_LOGI(TAG, "Waiting for playback thread to finish (timeout: 3s)");
        auto start_time = std::chrono::steady_clock::now();

        bool thread_finished = false;
        while (!thread_finished)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - start_time)
                               .count();

            if (elapsed >= 3)
            {
                ESP_LOGW(TAG, "Playback thread join timeout after 3 seconds");
                break;
            }

            // 再次设置停止标志
            is_playing_ = false;

            // 通知条件变量
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }

            // 检查线程是否已经结束
            if (!play_thread_.joinable())
            {
                thread_finished = true;
            }
        }

        if (play_thread_.joinable())
        {
            play_thread_.join();
        }
        ESP_LOGI(TAG, "Playback thread finished");
    }

    // 等待歌词线程结束
    if (lyric_thread_.joinable())
    {
        ESP_LOGI(TAG, "Waiting for lyric thread to finish");
        lyric_thread_.join();
        ESP_LOGI(TAG, "Lyric thread finished");
    }

    // 清理缓冲区和MP3解码器
    ClearAudioBuffer();
    CleanupMp3Decoder();

    ESP_LOGI(TAG, "Music player destroyed successfully");
}

void Esp32Music::PlayNextDetect()
{
    while (true)
    {
        std::unique_lock<std::mutex> lock(next_mutex_);
        // 等待直到 need_to_play_next_ 为 true
        play_next_cv_.wait(lock, [this]
                           { return need_to_play_next_; });
        need_to_play_next_ = false;
        // 释放锁，允许其他线程修改 need_to_play_next_
        lock.unlock();
        playNextSong();
        // 重新获取锁
        lock.lock();
        // 检查是否有新的播放请求
        if (need_to_play_next_)
        {
            continue; // 如果有新的请求，直接跳过重置步骤
        }
    }
}

bool Esp32Music::Download(const std::string &song_name)
{
    ESP_LOGI(TAG, "Starting to get music details for: %s", song_name.c_str());

    // 清空之前的下载数据
    last_downloaded_data_.clear();

    // 保存歌名用于后续显示
    current_song_name_ = song_name;
    // 第一步：请求stream_pcm接口获取音频信息
    std::string full_url = "https://search.kuwo.cn/r.s?pn=0&rn=3&all=" + url_encode(song_name) + "&ft=music&newsearch=1&alflac=1&itemset=web_2013&client=kt&cluster=0&vermerge=1&rformat=json&encoding=utf8&show_copyright_off=1&pcmp4=1&ver=mbox&plat=pc&vipver=MUSIC_9.1.1.2_BCS2&devid=38668888&newver=1&issubtitle=1&pcjson=1";
    ESP_LOGI(TAG, "Request URL: %s", full_url.c_str());

    // 使用Board提供的HTTP客户端
    auto http = Board::GetInstance().CreateHttp();
    // 打开GET连接
    if (!http->Open("GET", full_url))
    {
        ESP_LOGE(TAG, "Failed to connect to music API");
        return false;
    }

    // 检查响应状态码
    int status_code = http->GetStatusCode();
    if (status_code != 200)
    {
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http->Close();
        return false;
    }

    // 读取响应数据
    std::string body(30720, '\0');
    int bytes_read = http->Read(body.data(), 30720);
    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, bytes_read);
    http->Close();
    if (bytes_read > 0)
    {
        last_downloaded_data_ = body.substr(0, bytes_read);
        // 解析响应JSON以提取音频URL
        cJSON *response_json = cJSON_Parse(last_downloaded_data_.c_str());
        if (!response_json)
        {
            return false;
        }

        // 获取abslist数组
        cJSON *abslist = cJSON_GetObjectItem(response_json, "abslist");
        if (!cJSON_IsArray(abslist) || cJSON_GetArraySize(abslist) == 0)
        {
            ESP_LOGE(TAG, "can not get song info from json!");
            cJSON_Delete(response_json);
            return false;
        }

        // 获取第一个歌曲对象
        cJSON *firstSong = cJSON_GetArrayItem(abslist, 0);
        cJSON *targetId = cJSON_GetObjectItem(firstSong, "DC_TARGETID");

        if (!targetId || !targetId->valuestring)
        {
            ESP_LOGE(TAG, "未找到歌曲ID");
            cJSON_Delete(response_json);
            return false;
        }

        ESP_LOGI(TAG, "歌曲ID: %s", targetId->valuestring);
        string songId = string(targetId->valuestring);
        string url = KwWork::getUrl(songId);
        ESP_LOGI(TAG, "url = %s", url.c_str());
        cJSON_Delete(response_json);
        // 打开GET连接
        current_music_url_ = this->getSongPlayUrl(url);
        if (current_music_url_.empty())
        {
            ESP_LOGE(TAG, "Failed to get song play url");
            return false;
        }
        ESP_LOGI(TAG, "songUrl = %s", current_music_url_.c_str());
        ESP_LOGI(TAG, "Starting streaming playback for: %s", song_name.c_str());
        StartStreaming(current_music_url_);

        // 处理歌词URL
        if (!songId.empty())
        {
            // current_lyric_url_ = "https://www.kuwo.cn/openapi/v1/www/lyric/getlyric?musicId=" + songId;
            current_lyric_url_ = "https://www.kuwo.cn/newh5/singles/songinfoandlrc?musicId=" + songId;
            ESP_LOGI(TAG, "Loading lyrics for: %s", song_name.c_str());
            // 启动歌词下载和显示
            if (is_lyric_running_)
            {
                is_lyric_running_ = false;
                if (lyric_thread_.joinable())
                {
                    lyric_thread_.join();
                }
            }

            is_lyric_running_ = true;
            current_lyric_index_ = -1;
            lyrics_.clear();

            lyric_thread_ = std::thread(&Esp32Music::LyricDisplayThread, this);
        }
        else
        {
            ESP_LOGW(TAG, "No lyric URL found for this song");
        }
        return true;
    }
    else
    {
        ESP_LOGE(TAG, "Empty response from music API");
    }

    return false;
}

bool Esp32Music::playNextSong()
{
    if (play_next_.empty())
    {
        ESP_LOGE(TAG, "song_id is empty");
        return false;
    }
    ESP_LOGI(TAG, "歌曲ID: %s", play_next_.c_str());
    string url = KwWork::getUrl(play_next_);
    ESP_LOGI(TAG, "url = %s", url.c_str());
    current_music_url_ = this->getSongPlayUrl(url);
    if (current_music_url_.empty())
    {
        ESP_LOGE(TAG, "Failed to get song play url");
        return false;
    }
    ESP_LOGI(TAG, "songUrl = %s", current_music_url_.c_str());
    StartStreaming(current_music_url_);
    // current_lyric_url_ = "https://www.kuwo.cn/openapi/v1/www/lyric/getlyric?musicId=" + songId;
    current_lyric_url_ = "https://www.kuwo.cn/newh5/singles/songinfoandlrc?musicId=" + play_next_;
    // 启动歌词下载和显示
    if (is_lyric_running_)
    {
        is_lyric_running_ = false;
        if (lyric_thread_.joinable())
        {
            lyric_thread_.join();
        }
    }

    is_lyric_running_ = true;
    current_lyric_index_ = -1;
    lyrics_.clear();
    lyric_thread_ = std::thread(&Esp32Music::LyricDisplayThread, this);
    return true;
}

std::string Esp32Music::getSongPlayUrl(const std::string &req)
{
    auto http = Board::GetInstance().CreateHttp();
    if (!http->Open("GET", req))
    {
        ESP_LOGE(TAG, "Failed to connect to music API");
        return "";
    }

    // 检查响应状态码
    int status_code = http->GetStatusCode();
    if (status_code != 200)
    {
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http->Close();
        return "";
    }
    // 读取响应数据
    std::string rsp = "";
    std::string body(1024, '\0');
    int bytes_read = http->Read(body.data(), 1024);
    http->Close();
    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d, body = %s", status_code, bytes_read, body.c_str());
    if (bytes_read > 0)
    {
        rsp = body.substr(0, bytes_read);
        vector<string> lines;
        istringstream iss(rsp.c_str());
        string line;
        while (getline(iss, line))
        {
            lines.push_back(line);
        }
        if (lines.size() > 2)
        {
            string mu = lines[2];
            size_t pos = mu.find(".mp3");
            if (pos != std::string::npos)
            {
                mu = mu.substr(4, pos);
            }
            else
            {
                mu = mu.substr(4);
            }
            return std::string(mu.c_str());
        }
    }
    return "";
}

bool Esp32Music::Play()
{
    if (is_playing_.load())
    { // 使用atomic的load()
        ESP_LOGW(TAG, "Music is already playing");
        return true;
    }

    if (last_downloaded_data_.empty())
    {
        ESP_LOGE(TAG, "No music data to play");
        return false;
    }

    // 清理之前的播放线程
    if (play_thread_.joinable())
    {
        play_thread_.join();
    }

    // 实际应调用流式播放接口
    return StartStreaming(current_music_url_);
}

bool Esp32Music::Stop()
{
    if (!is_playing_ && !is_downloading_)
    {
        ESP_LOGW(TAG, "Music is not playing or downloading");
        return true;
    }

    ESP_LOGI(TAG, "Stopping music playback and download");

    // 停止下载和播放
    is_downloading_ = false;
    is_playing_ = false;

    // 重置采样率到原始值
    ResetSampleRate();

    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    // 等待线程结束
    if (download_thread_.joinable())
    {
        download_thread_.join();
    }
    if (play_thread_.joinable())
    {
        play_thread_.join();
    }

    // 清空缓冲区
    ClearAudioBuffer();

    ESP_LOGI(TAG, "Music stopped successfully");
    return true;
}

std::string Esp32Music::GetDownloadResult()
{
    return last_downloaded_data_;
}

// 开始流式播放
bool Esp32Music::StartStreaming(const std::string &music_url)
{
    if (music_url.empty())
    {
        ESP_LOGE(TAG, "Music URL is empty");
        return false;
    }

    ESP_LOGD(TAG, "Starting streaming for URL: %s", music_url.c_str());

    // 停止之前的播放和下载
    is_downloading_ = false;
    is_playing_ = false;

    // 等待之前的线程完全结束
    if (download_thread_.joinable())
    {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all(); // 通知线程退出
        }
        download_thread_.join();
    }
    if (play_thread_.joinable())
    {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all(); // 通知线程退出
        }
        play_thread_.join();
    }

    // 清空缓冲区
    ClearAudioBuffer();

    // 配置线程栈大小以避免栈溢出
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 8192; // 8KB栈大小
    cfg.prio = 5;          // 中等优先级
    cfg.thread_name = "audio_stream";
    esp_pthread_set_cfg(&cfg);

    // 开始下载线程
    is_downloading_ = true;
    download_thread_ = std::thread(&Esp32Music::DownloadAudioStream, this, music_url);

    // 开始播放线程（会等待缓冲区有足够数据）
    is_playing_ = true;
    play_thread_ = std::thread(&Esp32Music::PlayAudioStream, this);

    ESP_LOGI(TAG, "Streaming threads started successfully");
    return true;
}

// 停止流式播放
bool Esp32Music::StopStreaming()
{
    ESP_LOGI(TAG, "Stopping music streaming - current state: downloading=%d, playing=%d",
             is_downloading_.load(), is_playing_.load());

    // 重置采样率到原始值
    ResetSampleRate();

    // 检查是否有流式播放正在进行
    if (!is_playing_ && !is_downloading_)
    {
        ESP_LOGW(TAG, "No streaming in progress");
        return true;
    }

    // 停止下载和播放标志
    is_downloading_ = false;
    is_playing_ = false;

    // 清空歌名显示
    auto &board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display)
    {
        display->SetMusicInfo(""); // 清空歌名显示
        ESP_LOGI(TAG, "Cleared song name display");
    }

    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    ESP_LOGI(TAG, "Music streaming stop signal sent");
    return true;
}

// 流式下载音频数据
void Esp32Music::DownloadAudioStream(const std::string &music_url)
{
    ESP_LOGD(TAG, "Starting audio stream download from: %s", music_url.c_str());

    // 验证URL有效性
    if (music_url.empty() || music_url.find("http") != 0)
    {
        ESP_LOGE(TAG, "Invalid URL format: %s", music_url.c_str());
        is_downloading_ = false;
        return;
    }

    auto http = Board::GetInstance().CreateHttp();

    // 设置请求头
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Range", "bytes=0-"); // 支持断点续传

    if (!http->Open("GET", music_url))
    {
        ESP_LOGE(TAG, "Failed to connect to music stream URL");
        is_downloading_ = false;
        return;
    }

    int status_code = http->GetStatusCode();
    if (status_code != 200 && status_code != 206)
    { // 206 for partial content
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http->Close();
        is_downloading_ = false;
        return;
    }

    ESP_LOGI(TAG, "Started downloading audio stream, status: %d", status_code);

    // 分块读取音频数据
    const size_t chunk_size = 4096; // 4KB每块
    char buffer[chunk_size];
    size_t total_downloaded = 0;

    while (is_downloading_ && is_playing_)
    {
        int bytes_read = http->Read(buffer, chunk_size);
        if (bytes_read < 0)
        {
            ESP_LOGE(TAG, "Failed to read audio data: error code %d", bytes_read);
            break;
        }
        if (bytes_read == 0)
        {
            ESP_LOGI(TAG, "Audio stream download completed, total: %d bytes", total_downloaded);
            break;
        }

        // 打印数据块信息
        // ESP_LOGI(TAG, "Downloaded chunk: %d bytes at offset %d", bytes_read, total_downloaded);

        // 安全地打印数据块的十六进制内容（前16字节）
        if (bytes_read >= 16)
        {
            // ESP_LOGI(TAG, "Data: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X ...",
            //         (unsigned char)buffer[0], (unsigned char)buffer[1], (unsigned char)buffer[2], (unsigned char)buffer[3],
            //         (unsigned char)buffer[4], (unsigned char)buffer[5], (unsigned char)buffer[6], (unsigned char)buffer[7],
            //         (unsigned char)buffer[8], (unsigned char)buffer[9], (unsigned char)buffer[10], (unsigned char)buffer[11],
            //         (unsigned char)buffer[12], (unsigned char)buffer[13], (unsigned char)buffer[14], (unsigned char)buffer[15]);
        }
        else
        {
            ESP_LOGI(TAG, "Data chunk too small: %d bytes", bytes_read);
        }

        // 尝试检测文件格式（检查文件头）
        if (total_downloaded == 0 && bytes_read >= 4)
        {
            if (memcmp(buffer, "ID3", 3) == 0)
            {
                ESP_LOGI(TAG, "Detected MP3 file with ID3 tag");
            }
            else if (buffer[0] == 0xFF && (buffer[1] & 0xE0) == 0xE0)
            {
                ESP_LOGI(TAG, "Detected MP3 file header");
            }
            else if (memcmp(buffer, "RIFF", 4) == 0)
            {
                ESP_LOGI(TAG, "Detected WAV file");
            }
            else if (memcmp(buffer, "fLaC", 4) == 0)
            {
                ESP_LOGI(TAG, "Detected FLAC file");
            }
            else if (memcmp(buffer, "OggS", 4) == 0)
            {
                ESP_LOGI(TAG, "Detected OGG file");
            }
            else
            {
                ESP_LOGI(TAG, "Unknown audio format, first 4 bytes: %02X %02X %02X %02X",
                         (unsigned char)buffer[0], (unsigned char)buffer[1],
                         (unsigned char)buffer[2], (unsigned char)buffer[3]);
            }
        }

        // 创建音频数据块
        uint8_t *chunk_data = (uint8_t *)heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
        if (!chunk_data)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for audio chunk");
            break;
        }
        memcpy(chunk_data, buffer, bytes_read);

        // 等待缓冲区有空间
        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            buffer_cv_.wait(lock, [this]
                            { return buffer_size_ < MAX_BUFFER_SIZE || !is_downloading_; });

            if (is_downloading_)
            {
                audio_buffer_.push(AudioChunk(chunk_data, bytes_read));
                buffer_size_ += bytes_read;
                total_downloaded += bytes_read;

                // 通知播放线程有新数据
                buffer_cv_.notify_one();

                if (total_downloaded % (256 * 1024) == 0)
                { // 每256KB打印一次进度
                    ESP_LOGI(TAG, "Downloaded %d bytes, buffer size: %d", total_downloaded, buffer_size_);
                }
            }
            else
            {
                heap_caps_free(chunk_data);
                break;
            }
        }
    }

    http->Close();
    is_downloading_ = false;

    // 通知播放线程下载完成
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    ESP_LOGI(TAG, "Audio stream download thread finished");
}

// 流式播放音频数据
void Esp32Music::PlayAudioStream()
{
    ESP_LOGI(TAG, "Starting audio stream playback");

    // 初始化时间跟踪变量
    current_play_time_ms_ = 0;
    last_frame_time_ms_ = 0;
    total_frames_decoded_ = 0;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec || !codec->output_enabled())
    {
        ESP_LOGE(TAG, "Audio codec not available or not enabled");
        is_playing_ = false;
        return;
    }

    if (!mp3_decoder_initialized_)
    {
        ESP_LOGE(TAG, "MP3 decoder not initialized");
        is_playing_ = false;
        return;
    }

    // 等待缓冲区有足够数据开始播放
    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);
        buffer_cv_.wait(lock, [this]
                        { return buffer_size_ >= MIN_BUFFER_SIZE || (!is_downloading_ && !audio_buffer_.empty()); });
    }
    ESP_LOGI(TAG, "Starting playback with buffer size: %d", buffer_size_);

    size_t total_played = 0;
    uint8_t *mp3_input_buffer = nullptr;
    int bytes_left = 0;
    uint8_t *read_ptr = nullptr;

    // 分配MP3输入缓冲区
    mp3_input_buffer = (uint8_t *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!mp3_input_buffer)
    {
        ESP_LOGE(TAG, "Failed to allocate MP3 input buffer");
        is_playing_ = false;
        return;
    }

    // 标记是否已经处理过ID3标签
    bool id3_processed = false;

    while (is_playing_)
    {
        // 检查设备状态，只有在空闲状态才播放音乐
        auto &app = Application::GetInstance();
        DeviceState current_state = app.GetDeviceState();

        // 等小智把话说完了，变成聆听状态之后，马上转成待机状态，进入音乐播放
        if (current_state == kDeviceStateListening)
        {
            ESP_LOGI(TAG, "Device is in listening state, switching to idle state for music playback");
            // 切换状态
            app.ToggleChatState(); // 变成待机状态
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        else if (current_state != kDeviceStateIdle)
        { // 不是待机状态，就一直卡在这里，不让播放音乐
            ESP_LOGD(TAG, "Device state is %d, pausing music playback", current_state);
            // 如果不是空闲状态，暂停播放
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // 设备状态检查通过，显示当前播放的歌名
        if (!song_name_displayed_ && !current_song_name_.empty())
        {
            auto &board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display)
            {
                // 格式化歌名显示为《歌名》播放中...
                std::string formatted_song_name = "《" + current_song_name_ + "》播放中...";
                display->SetMusicInfo(formatted_song_name.c_str());
                ESP_LOGI(TAG, "Displaying song name: %s", formatted_song_name.c_str());
                song_name_displayed_ = true;
            }
        }

        // 如果需要更多MP3数据，从缓冲区读取
        if (bytes_left < 4096)
        { // 保持至少4KB数据用于解码
            AudioChunk chunk;

            // 从缓冲区获取音频数据
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                if (audio_buffer_.empty())
                {
                    if (!is_downloading_)
                    {
                        // 下载完成且缓冲区为空，播放结束
                        ESP_LOGI(TAG, "Playback finished, total played: %d bytes", total_played);
                        break;
                    }
                    // 等待新数据
                    buffer_cv_.wait(lock, [this]
                                    { return !audio_buffer_.empty() || !is_downloading_; });
                    if (audio_buffer_.empty())
                    {
                        continue;
                    }
                }

                chunk = audio_buffer_.front();
                audio_buffer_.pop();
                buffer_size_ -= chunk.size;

                // 通知下载线程缓冲区有空间
                buffer_cv_.notify_one();
            }

            // 将新数据添加到MP3输入缓冲区
            if (chunk.data && chunk.size > 0)
            {
                // 移动剩余数据到缓冲区开头
                if (bytes_left > 0 && read_ptr != mp3_input_buffer)
                {
                    memmove(mp3_input_buffer, read_ptr, bytes_left);
                }

                // 检查缓冲区空间
                size_t space_available = 8192 - bytes_left;
                size_t copy_size = std::min(chunk.size, space_available);

                // 复制新数据
                memcpy(mp3_input_buffer + bytes_left, chunk.data, copy_size);
                bytes_left += copy_size;
                read_ptr = mp3_input_buffer;

                // 检查并跳过ID3标签（仅在开始时处理一次）
                if (!id3_processed && bytes_left >= 10)
                {
                    size_t id3_skip = SkipId3Tag(read_ptr, bytes_left);
                    if (id3_skip > 0)
                    {
                        read_ptr += id3_skip;
                        bytes_left -= id3_skip;
                        ESP_LOGI(TAG, "Skipped ID3 tag: %u bytes", (unsigned int)id3_skip);
                    }
                    id3_processed = true;
                }

                // 释放chunk内存
                heap_caps_free(chunk.data);
            }
        }

        // 尝试找到MP3帧同步
        int sync_offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (sync_offset < 0)
        {
            ESP_LOGW(TAG, "No MP3 sync word found, skipping %d bytes", bytes_left);
            bytes_left = 0;
            continue;
        }

        // 跳过到同步位置
        if (sync_offset > 0)
        {
            read_ptr += sync_offset;
            bytes_left -= sync_offset;
        }

        // 解码MP3帧
        int16_t pcm_buffer[2304];
        int decode_result = MP3Decode(mp3_decoder_, &read_ptr, &bytes_left, pcm_buffer, 0);

        if (decode_result == 0)
        {
            // 解码成功，获取帧信息
            MP3GetLastFrameInfo(mp3_decoder_, &mp3_frame_info_);
            total_frames_decoded_++;

            // 基本的帧信息有效性检查，防止除零错误
            if (mp3_frame_info_.samprate == 0 || mp3_frame_info_.nChans == 0)
            {
                ESP_LOGW(TAG, "Invalid frame info: rate=%d, channels=%d, skipping",
                         mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                continue;
            }

            // 计算当前帧的持续时间(毫秒)
            int frame_duration_ms = (mp3_frame_info_.outputSamps * 1000) /
                                    (mp3_frame_info_.samprate * mp3_frame_info_.nChans);

            // 更新当前播放时间
            current_play_time_ms_ += frame_duration_ms;

            ESP_LOGD(TAG, "Frame %d: time=%lldms, duration=%dms, rate=%d, ch=%d",
                     total_frames_decoded_, current_play_time_ms_, frame_duration_ms,
                     mp3_frame_info_.samprate, mp3_frame_info_.nChans);

            // 更新歌词显示
            int buffer_latency_ms = 600; // 实测调整值
            UpdateLyricDisplay(current_play_time_ms_ + buffer_latency_ms);

            // 将PCM数据发送到Application的音频解码队列
            if (mp3_frame_info_.outputSamps > 0)
            {
                int16_t *final_pcm_data = pcm_buffer;
                int final_sample_count = mp3_frame_info_.outputSamps;
                std::vector<int16_t> mono_buffer;

                // 如果是双通道，转换为单通道混合
                if (mp3_frame_info_.nChans == 2)
                {
                    // 双通道转单通道：将左右声道混合
                    int stereo_samples = mp3_frame_info_.outputSamps; // 包含左右声道的总样本数
                    int mono_samples = stereo_samples / 2;            // 实际的单声道样本数

                    mono_buffer.resize(mono_samples);

                    for (int i = 0; i < mono_samples; ++i)
                    {
                        // 混合左右声道 (L + R) / 2
                        int left = pcm_buffer[i * 2];      // 左声道
                        int right = pcm_buffer[i * 2 + 1]; // 右声道
                        mono_buffer[i] = (int16_t)((left + right) / 2);
                    }

                    final_pcm_data = mono_buffer.data();
                    final_sample_count = mono_samples;

                    ESP_LOGD(TAG, "Converted stereo to mono: %d -> %d samples",
                             stereo_samples, mono_samples);
                }
                else if (mp3_frame_info_.nChans == 1)
                {
                    // 已经是单声道，无需转换
                    ESP_LOGD(TAG, "Already mono audio: %d samples", final_sample_count);
                }
                else
                {
                    ESP_LOGW(TAG, "Unsupported channel count: %d, treating as mono",
                             mp3_frame_info_.nChans);
                }

                // 创建AudioStreamPacket
                AudioStreamPacket packet;
                packet.sample_rate = mp3_frame_info_.samprate;
                packet.frame_duration = 60; // 使用Application默认的帧时长
                packet.timestamp = 0;

                // 将int16_t PCM数据转换为uint8_t字节数组
                size_t pcm_size_bytes = final_sample_count * sizeof(int16_t);
                packet.payload.resize(pcm_size_bytes);
                memcpy(packet.payload.data(), final_pcm_data, pcm_size_bytes);

                ESP_LOGD(TAG, "Sending %d PCM samples (%d bytes, rate=%d, channels=%d->1) to Application",
                         final_sample_count, pcm_size_bytes, mp3_frame_info_.samprate, mp3_frame_info_.nChans);

                // 发送到Application的音频解码队列
                app.AddAudioData(std::move(packet));
                total_played += pcm_size_bytes;

                // 打印播放进度
                if (total_played % (128 * 1024) == 0)
                {
                    ESP_LOGI(TAG, "Played %d bytes, buffer size: %d", total_played, buffer_size_);
                }
            }
        }
        else
        {
            // 解码失败
            ESP_LOGW(TAG, "MP3 decode failed with error: %d", decode_result);

            // 跳过一些字节继续尝试
            if (bytes_left > 1)
            {
                read_ptr++;
                bytes_left--;
            }
            else
            {
                bytes_left = 0;
            }
        }
    }

    // 清理
    if (mp3_input_buffer)
    {
        heap_caps_free(mp3_input_buffer);
    }

    // 播放结束时清空歌名显示
    auto &board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display)
    {
        display->SetMusicInfo(""); // 清空歌名显示
        ESP_LOGI(TAG, "Cleared song name display on playback end");
    }

    // 重置采样率到原始值
    ResetSampleRate();

    // 播放结束时保持音频输出启用状态，让Application管理
    // 不在这里禁用音频输出，避免干扰其他音频功能
    ESP_LOGI(TAG, "Audio stream playback finished, total played: %d bytes", total_played);

    is_playing_ = false;
    need_to_play_next_ = true;
    {
        std::lock_guard<std::mutex> lock(next_mutex_);
        play_next_cv_.notify_all();
    }
}

// 清空音频缓冲区
void Esp32Music::ClearAudioBuffer()
{
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    while (!audio_buffer_.empty())
    {
        AudioChunk chunk = audio_buffer_.front();
        audio_buffer_.pop();
        if (chunk.data)
        {
            heap_caps_free(chunk.data);
        }
    }

    buffer_size_ = 0;
    ESP_LOGI(TAG, "Audio buffer cleared");
}

// 初始化MP3解码器
bool Esp32Music::InitializeMp3Decoder()
{
    mp3_decoder_ = MP3InitDecoder();
    if (mp3_decoder_ == nullptr)
    {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        mp3_decoder_initialized_ = false;
        return false;
    }

    mp3_decoder_initialized_ = true;
    ESP_LOGI(TAG, "MP3 decoder initialized successfully");
    return true;
}

// 清理MP3解码器
void Esp32Music::CleanupMp3Decoder()
{
    if (mp3_decoder_ != nullptr)
    {
        MP3FreeDecoder(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
    mp3_decoder_initialized_ = false;
    ESP_LOGI(TAG, "MP3 decoder cleaned up");
}

// 重置采样率到原始值
void Esp32Music::ResetSampleRate()
{
    auto &board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    if (codec && codec->original_output_sample_rate() > 0 &&
        codec->output_sample_rate() != codec->original_output_sample_rate())
    {
        ESP_LOGI(TAG, "重置采样率：从 %d Hz 重置到原始值 %d Hz",
                 codec->output_sample_rate(), codec->original_output_sample_rate());
        if (codec->SetOutputSampleRate(-1))
        { // -1 表示重置到原始值
            ESP_LOGI(TAG, "成功重置采样率到原始值: %d Hz", codec->output_sample_rate());
        }
        else
        {
            ESP_LOGW(TAG, "无法重置采样率到原始值");
        }
    }
}

// 跳过MP3文件开头的ID3标签
size_t Esp32Music::SkipId3Tag(uint8_t *data, size_t size)
{
    if (!data || size < 10)
    {
        return 0;
    }

    // 检查ID3v2标签头 "ID3"
    if (memcmp(data, "ID3", 3) != 0)
    {
        return 0;
    }

    // 计算标签大小（synchsafe integer格式）
    uint32_t tag_size = ((uint32_t)(data[6] & 0x7F) << 21) |
                        ((uint32_t)(data[7] & 0x7F) << 14) |
                        ((uint32_t)(data[8] & 0x7F) << 7) |
                        ((uint32_t)(data[9] & 0x7F));

    // ID3v2头部(10字节) + 标签内容
    size_t total_skip = 10 + tag_size;

    // 确保不超过可用数据大小
    if (total_skip > size)
    {
        total_skip = size;
    }

    ESP_LOGI(TAG, "Found ID3v2 tag, skipping %u bytes", (unsigned int)total_skip);
    return total_skip;
}

// 下载歌词
bool Esp32Music::DownloadLyrics(const std::string &lyric_url)
{
    ESP_LOGI(TAG, "Downloading lyrics from: %s", lyric_url.c_str());

    // 检查URL是否为空
    if (lyric_url.empty())
    {
        ESP_LOGE(TAG, "Lyric URL is empty!");
        return false;
    }

    // 添加重试逻辑
    const int max_retries = 3;
    int retry_count = 0;
    bool success = false;
    std::string lyric_content;
    std::string current_url = lyric_url;
    int redirect_count = 0;
    const int max_redirects = 5; // 最多允许5次重定向

    while (retry_count < max_retries && !success && redirect_count < max_redirects)
    {
        if (retry_count > 0)
        {
            ESP_LOGI(TAG, "Retrying lyric download (attempt %d of %d)", retry_count + 1, max_retries);
            // 重试前暂停一下
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // 使用Board提供的HTTP客户端
        auto http = Board::GetInstance().CreateHttp();
        if (!http)
        {
            ESP_LOGE(TAG, "Failed to create HTTP client for lyric download");
            retry_count++;
            continue;
        }
        // 打开GET连接
        if (!http->Open("GET", current_url))
        {
            ESP_LOGE(TAG, "Failed to open HTTP connection for lyrics");
            delete http;
            retry_count++;
            continue;
        }

        // 检查HTTP状态码
        int status_code = http->GetStatusCode();
        ESP_LOGI(TAG, "Lyric download HTTP status code: %d", status_code);

        // 处理重定向 - 由于Http类没有GetHeader方法，我们只能根据状态码判断
        if (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308)
        {
            // 由于无法获取Location头，只能报告重定向但无法继续
            ESP_LOGW(TAG, "Received redirect status %d but cannot follow redirect (no GetHeader method)", status_code);
            http->Close();
            delete http;
            retry_count++;
            continue;
        }

        // 非200系列状态码视为错误
        if (status_code < 200 || status_code >= 300)
        {
            ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
            http->Close();
            delete http;
            retry_count++;
            continue;
        }

        // 读取响应
        lyric_content.clear();
        char buffer[1024];
        int bytes_read;
        bool read_error = false;
        int total_read = 0;

        // 由于无法获取Content-Length和Content-Type头，我们不知道预期大小和内容类型
        ESP_LOGD(TAG, "Starting to read lyric content");

        while (true)
        {
            bytes_read = http->Read(buffer, sizeof(buffer) - 1);
            // ESP_LOGD(TAG, "Lyric HTTP read returned %d bytes", bytes_read); // 注释掉以减少日志输出

            if (bytes_read > 0)
            {
                buffer[bytes_read] = '\0';
                lyric_content += buffer;
                total_read += bytes_read;

                // 定期打印下载进度 - 改为DEBUG级别减少输出
                if (total_read % 4096 == 0)
                {
                    ESP_LOGD(TAG, "Downloaded %d bytes so far", total_read);
                }
            }
            else if (bytes_read == 0)
            {
                // 正常结束，没有更多数据
                ESP_LOGD(TAG, "Lyric download completed, total bytes: %d", total_read);
                success = true;
                break;
            }
            else
            {
                // bytes_read < 0，可能是ESP-IDF的已知问题
                // 如果已经读取到了一些数据，则认为下载成功
                if (!lyric_content.empty())
                {
                    ESP_LOGW(TAG, "HTTP read returned %d, but we have data (%d bytes), continuing", bytes_read, lyric_content.length());
                    success = true;
                    break;
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to read lyric data: error code %d", bytes_read);
                    read_error = true;
                    break;
                }
            }
        }

        http->Close();
        delete http;

        if (read_error)
        {
            retry_count++;
            continue;
        }

        // 如果成功读取数据，跳出重试循环
        if (success)
        {
            break;
        }
    }

    // 检查是否超过了最大重试次数
    if (retry_count >= max_retries)
    {
        ESP_LOGE(TAG, "Failed to download lyrics after %d attempts", max_retries);
        return false;
    }

    // 记录前几个字节的数据，帮助调试
    if (!lyric_content.empty())
    {
        size_t preview_size = std::min(lyric_content.size(), size_t(50));
        std::string preview = lyric_content.substr(0, preview_size);
        ESP_LOGD(TAG, "Lyric content preview (%d bytes): %s", lyric_content.length(), preview.c_str());
    }
    else
    {
        ESP_LOGE(TAG, "Failed to download lyrics or lyrics are empty");
        return false;
    }

    ESP_LOGI(TAG, "Lyrics downloaded successfully, size: %d bytes", lyric_content.length());
    ParseRecommondSong(lyric_content);
    return ParseLyrics(lyric_content);
}

// 解析歌词
bool Esp32Music::ParseLyrics(const std::string &lyric_content)
{
    ESP_LOGD(TAG, "Parsing lyrics content");

    // 解析响应JSON以提取歌词
    cJSON *rsp = cJSON_Parse(lyric_content.c_str());
    if (!rsp)
    {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return false;
    }

    // 获取"data"对象
    cJSON *data = cJSON_GetObjectItem(rsp, "data");
    if (!data || !cJSON_IsObject(data))
    {
        ESP_LOGE(TAG, "Invalid JSON structure - 'data' object not found");
        cJSON_Delete(rsp);
        return false;
    }

    // 获取"lrclist"数组
    cJSON *lrclist = cJSON_GetObjectItem(data, "lrclist");
    if (!lrclist || !cJSON_IsArray(lrclist) || cJSON_GetArraySize(lrclist) == 0)
    {
        ESP_LOGE(TAG, "Cannot get 'lrclist' from JSON!");
        cJSON_Delete(rsp);
        return false;
    }

    // 预先分配内存以减少内存分配开销
    lyrics_.reserve(cJSON_GetArraySize(lrclist));

    for (int i = 0; i < cJSON_GetArraySize(lrclist); i++)
    {
        cJSON *lyric = cJSON_GetArrayItem(lrclist, i);
        cJSON *content = cJSON_GetObjectItem(lyric, "lineLyric");
        cJSON *timeStr = cJSON_GetObjectItem(lyric, "time");

        if (!content || !content->valuestring || !timeStr || !timeStr->valuestring)
        {
            ESP_LOGW(TAG, "Incomplete lyric data at index %d", i);
            continue;
        }

        float time = atof(timeStr->valuestring);
        int time_ms = static_cast<int>(time * 1000);
        std::string content_str = content->valuestring;
        lyrics_.emplace_back(time_ms, content_str);
    }

    if (lyrics_.empty())
    {
        ESP_LOGE(TAG, "Parsed lyrics are empty");
        cJSON_Delete(rsp);
        return false;
    }

    // 排序歌词以确保按时间顺序显示
    std::sort(lyrics_.begin(), lyrics_.end(), [](const std::pair<int, std::string> &a, const std::pair<int, std::string> &b)
              { return a.first < b.first; });

    ESP_LOGI(TAG, "Parsed %d lyric lines", lyrics_.size());
    cJSON_Delete(rsp);
    return true;
}

bool Esp32Music::ParseRecommondSong(const std::string &lyric_content)
{
    ESP_LOGI(TAG, "ParseRecommondSong");

    // 解析响应JSON以提取音频URL
    cJSON *rsp = cJSON_Parse(lyric_content.c_str());
    if (!rsp)
    {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return false;
    }

    // 获取"data"对象
    cJSON *data = cJSON_GetObjectItem(rsp, "data");
    if (!cJSON_IsObject(data))
    {
        ESP_LOGE(TAG, "Invalid JSON structure - 'data' object not found");
        cJSON_Delete(rsp);
        return false;
    }

    // 获取"simpl"对象
    cJSON *simpl = cJSON_GetObjectItem(data, "simpl");
    if (!cJSON_IsObject(simpl))
    {
        ESP_LOGE(TAG, "Invalid JSON structure - 'simpl' object not found");
        cJSON_Delete(rsp);
        return false;
    }

    // 获取"musiclist"数组
    cJSON *musiclist = cJSON_GetObjectItem(simpl, "musiclist");
    if (!cJSON_IsArray(musiclist) || cJSON_GetArraySize(musiclist) == 0)
    {
        ESP_LOGE(TAG, "can not get musiclist from json!");
        cJSON_Delete(rsp);
        return false;
    }

    for (int i = 0; i < cJSON_GetArraySize(musiclist); i++)
    {
        cJSON *lyric = cJSON_GetArrayItem(musiclist, i);
        cJSON *musicrId = cJSON_GetObjectItem(lyric, "musicrId");
        if (!musicrId || !musicrId->valuestring)
        {
            ESP_LOGE(TAG, "Invalid JSON structure - 'musicrId' value not found");
            continue;
        }

        const std::string songId(musicrId->valuestring);
        if (songs_played_.find(songId) == songs_played_.end())
        {
            songs_played_.insert(songId);
            play_next_ = songId;
            ESP_LOGI(TAG, "ParseRecommondSong: play next song: %s", songId.c_str());
            cJSON_Delete(rsp);
            return true;
        }
    }

    cJSON_Delete(rsp);
    ESP_LOGW(TAG, "No new songs to play found in the recommend list");
    return false;
}

// 歌词显示线程
void Esp32Music::LyricDisplayThread()
{
    ESP_LOGI(TAG, "Lyric display thread started");

    if (!DownloadLyrics(current_lyric_url_))
    {
        ESP_LOGE(TAG, "Failed to download or parse lyrics");
        is_lyric_running_ = false;
        return;
    }

    // 定期检查是否需要更新显示(频率可以降低)
    while (is_lyric_running_ && is_playing_)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ESP_LOGI(TAG, "Lyric display thread finished");
}

void Esp32Music::UpdateLyricDisplay(int64_t current_time_ms)
{
    std::lock_guard<std::mutex> lock(lyrics_mutex_);

    if (lyrics_.empty())
    {
        return;
    }

    // 查找当前应该显示的歌词
    int new_lyric_index = -1;

    // 从当前歌词索引开始查找，提高效率
    int start_index = (current_lyric_index_.load() >= 0) ? current_lyric_index_.load() : 0;

    // 正向查找：找到最后一个时间戳小于等于当前时间的歌词
    for (int i = start_index; i < (int)lyrics_.size(); i++)
    {
        if (lyrics_[i].first <= current_time_ms)
        {
            new_lyric_index = i;
        }
        else
        {
            break; // 时间戳已超过当前时间
        }
    }

    // 如果没有找到(可能当前时间比第一句歌词还早)，显示空
    if (new_lyric_index == -1)
    {
        new_lyric_index = -1;
    }

    // 如果歌词索引发生变化，更新显示
    if (new_lyric_index != current_lyric_index_)
    {
        current_lyric_index_ = new_lyric_index;

        auto &board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display)
        {
            std::string lyric_text;

            if (current_lyric_index_ >= 0 && current_lyric_index_ < (int)lyrics_.size())
            {
                lyric_text = lyrics_[current_lyric_index_].second;
            }

            // 显示歌词
            display->SetChatMessage("lyric", lyric_text.c_str());

            ESP_LOGD(TAG, "Lyric update at %lldms: %s",
                     current_time_ms,
                     lyric_text.empty() ? "(no lyric)" : lyric_text.c_str());
        }
    }
}