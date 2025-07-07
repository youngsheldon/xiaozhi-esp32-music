# 小智开源音乐固件

（中文 | [English](README_en.md) | [日本語](README_ja.md)）

## 视频

👉 [【开源】虾哥ai小智机器音乐播放器纯固件带歌词显示](https://www.bilibili.com/video/BV19oM4zqEiz)

👉 [【开源】虾哥小智音乐播放器纯固件](https://www.bilibili.com/video/BV1RqMEzEEp1)

## 介绍

这是一个由虾哥开源的[ESP32项目](https://github.com/78/xiaozhi-esp32)，以 MIT 许可证发布，允许任何人免费使用，或用于商业用途。

我们希望通过这个项目，让大家的小智都能播放歌曲。

如果你有任何想法或建议，请随时提出 Issues 或加入 QQ 群：826072986

项目主要贡献者：空白泡泡糖果（B站UP），硅灵造物科技（B站UP）

音乐服务器提供者（为爱发电）：蔓延科技

### 💡注意事项

#### 1. 如果小智说找不到歌曲怎么办？
进入[小智后台](https://xiaozhi.me/)，找到对应设备，修改角色配置
- 选择 DeepSeekV3 大语言模型
- 在人物介绍中填入
  - 收到音乐相关的需求时，只使用 MPC tool `self.music.play_song` 工具，同时禁止使用 `search_music` 功能。

#### 2. 歌曲播放时唤醒词不生效怎么办？
把`main\application.cc`文件的`AddAudioData`方法改成如下代码
```
void Application::AddAudioData(AudioStreamPacket&& packet) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (device_state_ == kDeviceStateIdle && codec->output_enabled()) {
        // packet.payload包含的是原始PCM数据（int16_t）
        if (packet.payload.size() >= 2) {
            size_t num_samples = packet.payload.size() / sizeof(int16_t);
            std::vector<int16_t> pcm_data(num_samples);
            memcpy(pcm_data.data(), packet.payload.data(), packet.payload.size());
            
            // 检查采样率是否匹配，如果不匹配则进行简单重采样
            if (packet.sample_rate != codec->output_sample_rate()) {
                // ESP_LOGI(TAG, "Resampling music audio from %d to %d Hz", 
                //         packet.sample_rate, codec->output_sample_rate());
                
                // 验证采样率参数
                if (packet.sample_rate <= 0 || codec->output_sample_rate() <= 0) {
                    ESP_LOGE(TAG, "Invalid sample rates: %d -> %d", 
                            packet.sample_rate, codec->output_sample_rate());
                    return;
                }
                
                std::vector<int16_t> resampled;
                
                // 使用浮点数计算精确的重采样比率Add commentMore actions
                float ratio = static_cast<float>(packet.sample_rate) / codec->output_sample_rate();
                
                if (packet.sample_rate > codec->output_sample_rate()) {
                    // 降采样：按精确比率跳跃采样
                    size_t expected_size = static_cast<size_t>(pcm_data.size() / ratio + 0.5f);
                    resampled.reserve(expected_size);
                    
                    for (float i = 0; i < pcm_data.size(); i += ratio) {
                        size_t index = static_cast<size_t>(i + 0.5f);  // 四舍五入
                        if (index < pcm_data.size()) {
                            resampled.push_back(pcm_data[index]);
                        }
                    }
                    
                    ESP_LOGD(TAG, "Downsampled %d -> %d samples (ratio: %.3f)", 
                            pcm_data.size(), resampled.size(), ratio);
                            
                } else {
                    // 上采样：线性插值
                    float upsample_ratio = codec->output_sample_rate() / static_cast<float>(packet.sample_rate);
                    size_t expected_size = static_cast<size_t>(pcm_data.size() * upsample_ratio + 0.5f);
                    resampled.reserve(expected_size);
                    
                    for (size_t i = 0; i < pcm_data.size(); ++i) {
                        // 添加原始样本
                        resampled.push_back(pcm_data[i]);
                        
                        // 计算需要插值的样本数
                        int interpolation_count = static_cast<int>(upsample_ratio) - 1;
                        if (interpolation_count > 0 && i + 1 < pcm_data.size()) {
                            int16_t current = pcm_data[i];
                            int16_t next = pcm_data[i + 1];
                            for (int j = 1; j <= interpolation_count; ++j) {
                                float t = static_cast<float>(j) / (interpolation_count + 1);
                                int16_t interpolated = static_cast<int16_t>(current + (next - current) * t);
                                resampled.push_back(interpolated);
                            }
                        } else if (interpolation_count > 0) {
                            // 最后一个样本，直接重复
                            for (int j = 1; j <= interpolation_count; ++j) {
                                resampled.push_back(pcm_data[i]);
                            }
                        }
                    }
                    
                    ESP_LOGI(TAG, "Upsampled %d -> %d samples (ratio: %.2f)", 
                            pcm_data.size(), resampled.size(), upsample_ratio);
                }
                
                pcm_data = std::move(resampled);
            }
            
            // 确保音频输出已启用
            if (!codec->output_enabled()) {
                codec->EnableOutput(true);
            }
            
            // 发送PCM数据到音频编解码器
            codec->OutputData(pcm_data);
            
            // 更新最后输出时间，防止OnAudioOutput自动禁用音频
            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_output_time_ = std::chrono::steady_clock::now();
            }
        }
    }
}
```


#### 3. 暂不支持的开发板
- ESP32C3芯片的开发板

### 项目改动范围

#### 新增
- main\boards\common\esp32_music.cc
- main\boards\common\esp32_music.h

#### 修改
- main\mcp_server.cc
- main\boards\common\board.cc
- main\boards\common\board.h
- main\application.cc
- main\application.h
- main\display\display.cc
- main\display\display.h
- main\idf_component.yml



### 基于 MCP 控制万物

小智 AI 聊天机器人作为一个语音交互入口，利用 Qwen / DeepSeek 等大模型的 AI 能力，通过 MCP 协议实现多端控制。

![通过MCP控制万物](docs/mcp-based-graph.jpg)

### 已实现功能

- 🎭 **丰富的角色定制系统**：支持台湾女友、土豆子、English Tutor 等多种预设角色
- 🎨 **个性化配置**：自定义助手昵称、对话语言、角色音色和性格介绍
- 🎵 **智能音乐控制**：支持 `self.music.play_song` 工具进行音乐播放控制
- 📡 Wi-Fi / ML307 Cat.1 4G 网络连接
- 🗣️ 离线语音唤醒 [ESP-SR](https://github.com/espressif/esp-sr)
- 🔗 支持两种通信协议（[Websocket](docs/websocket.md) 或 MQTT+UDP）
- 🎧 采用 OPUS 音频编解码
- 🤖 基于流式 ASR + LLM + TTS 架构的语音交互
- 👤 声纹识别，识别当前说话人的身份 [3D Speaker](https://github.com/modelscope/3D-Speaker)
- 📺 OLED / LCD 显示屏，支持表情显示
- 🔋 电量显示与电源管理
- 🌍 支持多语言（中文、英文、日文）
- 💻 支持 ESP32-C3、ESP32-S3、ESP32-P4 芯片平台
- 🏠 通过设备端 MCP 实现设备控制（音量、灯光、电机、GPIO 等）
- ☁️ 通过云端 MCP 扩展大模型能力（智能家居控制、PC桌面操作、知识搜索、邮件收发等）

## 硬件

### 面包板手工制作实践

详见飞书文档教程：

👉 [《小智 AI 聊天机器人百科全书》](https://ccnphfhqs21z.feishu.cn/wiki/F5krwD16viZoF0kKkvDcrZNYnhb?from=from_copylink)

面包板效果图如下：

![面包板效果图](docs/v1/wiring2.jpg)

### 支持 70 多个开源硬件（仅展示部分）

- <a href="https://oshwhub.com/li-chuang-kai-fa-ban/li-chuang-shi-zhan-pai-esp32-s3-kai-fa-ban" target="_blank" title="立创·实战派 ESP32-S3 开发板">立创·实战派 ESP32-S3 开发板</a>
- <a href="https://github.com/espressif/esp-box" target="_blank" title="乐鑫 ESP32-S3-BOX3">乐鑫 ESP32-S3-BOX3</a>
- <a href="https://docs.m5stack.com/zh_CN/core/CoreS3" target="_blank" title="M5Stack CoreS3">M5Stack CoreS3</a>
- <a href="https://docs.m5stack.com/en/atom/Atomic%20Echo%20Base" target="_blank" title="AtomS3R + Echo Base">M5Stack AtomS3R + Echo Base</a>
- <a href="https://gf.bilibili.com/item/detail/1108782064" target="_blank" title="神奇按钮 2.4">神奇按钮 2.4</a>
- <a href="https://www.waveshare.net/shop/ESP32-S3-Touch-AMOLED-1.8.htm" target="_blank" title="微雪电子 ESP32-S3-Touch-AMOLED-1.8">微雪电子 ESP32-S3-Touch-AMOLED-1.8</a>
- <a href="https://github.com/Xinyuan-LilyGO/T-Circle-S3" target="_blank" title="LILYGO T-Circle-S3">LILYGO T-Circle-S3</a>
- <a href="https://oshwhub.com/tenclass01/xmini_c3" target="_blank" title="虾哥 Mini C3">虾哥 Mini C3</a>
- <a href="https://oshwhub.com/movecall/cuican-ai-pendant-lights-up-y" target="_blank" title="Movecall CuiCan ESP32S3">璀璨·AI 吊坠</a>
- <a href="https://github.com/WMnologo/xingzhi-ai" target="_blank" title="无名科技Nologo-星智-1.54">无名科技 Nologo-星智-1.54TFT</a>
- <a href="https://www.seeedstudio.com/SenseCAP-Watcher-W1-A-p-5979.html" target="_blank" title="SenseCAP Watcher">SenseCAP Watcher</a>
- <a href="https://www.bilibili.com/video/BV1BHJtz6E2S/" target="_blank" title="ESP-HI 超低成本机器狗">ESP-HI 超低成本机器狗</a>

<div style="display: flex; justify-content: space-between;">
  <a href="docs/v1/lichuang-s3.jpg" target="_blank" title="立创·实战派 ESP32-S3 开发板">
    <img src="docs/v1/lichuang-s3.jpg" width="240" />
  </a>
  <a href="docs/v1/espbox3.jpg" target="_blank" title="乐鑫 ESP32-S3-BOX3">
    <img src="docs/v1/espbox3.jpg" width="240" />
  </a>
  <a href="docs/v1/m5cores3.jpg" target="_blank" title="M5Stack CoreS3">
    <img src="docs/v1/m5cores3.jpg" width="240" />
  </a>
  <a href="docs/v1/atoms3r.jpg" target="_blank" title="AtomS3R + Echo Base">
    <img src="docs/v1/atoms3r.jpg" width="240" />
  </a>
  <a href="docs/v1/magiclick.jpg" target="_blank" title="神奇按钮 2.4">
    <img src="docs/v1/magiclick.jpg" width="240" />
  </a>
  <a href="docs/v1/waveshare.jpg" target="_blank" title="微雪电子 ESP32-S3-Touch-AMOLED-1.8">
    <img src="docs/v1/waveshare.jpg" width="240" />
  </a>
  <a href="docs/v1/lilygo-t-circle-s3.jpg" target="_blank" title="LILYGO T-Circle-S3">
    <img src="docs/v1/lilygo-t-circle-s3.jpg" width="240" />
  </a>
  <a href="docs/v1/xmini-c3.jpg" target="_blank" title="虾哥 Mini C3">
    <img src="docs/v1/xmini-c3.jpg" width="240" />
  </a>
  <a href="docs/v1/movecall-cuican-esp32s3.jpg" target="_blank" title="CuiCan">
    <img src="docs/v1/movecall-cuican-esp32s3.jpg" width="240" />
  </a>
  <a href="docs/v1/wmnologo_xingzhi_1.54.jpg" target="_blank" title="无名科技Nologo-星智-1.54">
    <img src="docs/v1/wmnologo_xingzhi_1.54.jpg" width="240" />
  </a>
  <a href="docs/v1/sensecap_watcher.jpg" target="_blank" title="SenseCAP Watcher">
    <img src="docs/v1/sensecap_watcher.jpg" width="240" />
  </a>
  <a href="docs/v1/esp-hi.jpg" target="_blank" title="ESP-HI 超低成本机器狗">
    <img src="docs/v1/esp-hi.jpg" width="240" />
  </a>
</div>

## 软件

### 固件烧录

新手第一次操作建议先不要搭建开发环境，直接使用免开发环境烧录的固件。

固件默认接入 [xiaozhi.me](https://xiaozhi.me) 官方服务器，个人用户注册账号可以免费使用 Qwen 实时模型。

👉 [新手烧录固件教程](https://ccnphfhqs21z.feishu.cn/wiki/Zpz4wXBtdimBrLk25WdcXzxcnNS)

### 开发环境

- Cursor 或 VSCode
- 安装 ESP-IDF 插件，选择 SDK 版本 5.4 或以上
- Linux 比 Windows 更好，编译速度快，也免去驱动问题的困扰
- 本项目使用 Google C++ 代码风格，提交代码时请确保符合规范

### 开发者文档

- [自定义开发板指南](main/boards/README.md) - 学习如何为小智 AI 创建自定义开发板
- [MCP 协议物联网控制用法说明](docs/mcp-usage.md) - 了解如何通过 MCP 协议控制物联网设备
- [MCP 协议交互流程](docs/mcp-protocol.md) - 设备端 MCP 协议的实现方式
- [一份详细的 WebSocket 通信协议文档](docs/websocket.md)

## 大模型配置

如果你已经拥有一个的小智 AI 聊天机器人设备，并且已接入官方服务器，可以登录 [xiaozhi.me](https://xiaozhi.me) 控制台进行配置。

### 🎭 角色配置指南

在 [xiaozhi.me](https://xiaozhi.me) 控制台中，您可以：

1. **选择角色模板**：从台湾女友、土豆子、English Tutor、好奇小男孩、汪汪队队长等预设角色中选择
2. **设置助手昵称**：为您的 AI 伴侣起一个专属的名字（默认：小智）
3. **配置对话语言**：支持普通话、英语、日语等多种语言
4. **选择角色音色**：清澈小何等多种音色可供选择
5. **自定义角色介绍**：详细描述角色的性格特点和背景设定

💡 **特别功能**：收到音乐相关需求时，小智会优先使用 `self.music.play_song` 工具，确保音乐播放体验的流畅性。

👉 [后台操作视频教程（旧版界面）](https://www.bilibili.com/video/BV1jUCUY2EKM/)

## 相关开源项目

在个人电脑上部署服务器，可以参考以下第三方开源的项目：

- [xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) Python 服务器
- [joey-zhou/xiaozhi-esp32-server-java](https://github.com/joey-zhou/xiaozhi-esp32-server-java) Java 服务器
- [AnimeAIChat/xiaozhi-server-go](https://github.com/AnimeAIChat/xiaozhi-server-go) Golang 服务器

使用小智通信协议的第三方客户端项目：

- [huangjunsen0406/py-xiaozhi](https://github.com/huangjunsen0406/py-xiaozhi) Python 客户端
- [TOM88812/xiaozhi-android-client](https://github.com/TOM88812/xiaozhi-android-client) Android 客户端
- [100askTeam/xiaozhi-linux](http://github.com/100askTeam/xiaozhi-linux) 百问科技提供的 Linux 客户端
- [78/xiaozhi-sf32](https://github.com/78/xiaozhi-sf32) 思澈科技的蓝牙芯片固件
- [QuecPython/solution-xiaozhiAI](https://github.com/QuecPython/solution-xiaozhiAI) 移远提供的 QuecPython 固件
