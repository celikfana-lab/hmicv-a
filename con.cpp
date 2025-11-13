#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <sstream>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <cmath>
#include <iomanip>

// 🎬 VIDEO DECODING - FFMPEG LIBRARIES
extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/opt.h>
    #include <libswscale/swscale.h>
    #include <libswresample/swresample.h>
}

// 🎨 IMAGE DECODING
#define STBI_SUPPORT_WEBP
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// 🌐 WEBP SUPPORT
#include <webp/decode.h>

// 🎵 AUDIO DECODING
#include <mpg123.h>
#include <sndfile.h>

// 🚀 COMPRESSION
#include <zstd.h>

namespace fs = std::filesystem;

std::mutex cout_mutex;
std::atomic<int> processed_rows{0};
std::atomic<int> processed_frames{0};

// 🎨 RGBA STRUCT
struct RGBA {
    uint8_t r, g, b, a;
    
    bool operator<(const RGBA& other) const {
        if (r != other.r) return r < other.r;
        if (g != other.g) return g < other.g;
        if (b != other.b) return b < other.b;
        return a < other.a;
    }
    
    bool operator==(const RGBA& other) const {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
};

// 🎯 COMMAND STRUCT
struct Command {
    std::string cmd;
    int x, end_x, y;
    
    bool operator==(const Command& other) const {
        return cmd == other.cmd && x == other.x && end_x == other.end_x && y == other.y;
    }

    bool operator<(const Command& other) const {
        if (y != other.y) return y < other.y;
        if (x != other.x) return x < other.x;
        if (end_x != other.end_x) return end_x < other.end_x;
        return cmd < other.cmd;
    }
};

// 🎧 AUDIO DATA
struct AudioData {
    int sample_rate;
    int channels;
    int64_t total_samples;
    std::vector<std::vector<float>> channel_data;
};

// 🎬 VIDEO INFO
struct VideoInfo {
    int width, height;
    int fps_num, fps_den;
    int total_frames;
    bool has_audio;
};

// 🔥 FILE EXTENSION DETECTOR
std::string get_file_extension(const std::string& path) {
    size_t dot_pos = path.find_last_of('.');
    if (dot_pos == std::string::npos) return "";
    std::string ext = path.substr(dot_pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

// 🎯 FRAME RANGE STRING
std::string frames_to_range_string(const std::vector<int>& frames) {
    if (frames.empty()) return "";
    if (frames.size() == 1) return std::to_string(frames[0]);
    
    std::vector<std::string> ranges;
    int start = frames[0], end = frames[0];
    
    for (size_t i = 1; i < frames.size(); i++) {
        if (frames[i] == end + 1) {
            end = frames[i];
        } else {
            ranges.push_back(start == end ? std::to_string(start) : 
                           std::to_string(start) + "-" + std::to_string(end));
            start = frames[i];
            end = frames[i];
        }
    }
    ranges.push_back(start == end ? std::to_string(start) : 
                    std::to_string(start) + "-" + std::to_string(end));
    
    std::string result;
    for (size_t i = 0; i < ranges.size(); i++) {
        result += ranges[i];
        if (i < ranges.size() - 1) result += ",";
    }
    return result;
}

// 🎬 VIDEO FRAME EXTRACTOR USING FFMPEG
bool extract_video_frames(const std::string& path, VideoInfo& info, 
                         std::vector<std::vector<RGBA>>& frames_data,
                         AudioData* audio_out = nullptr) {
    
    std::cout << "🎬 FFMPEG VIDEO DECODER ACTIVATED!! 🔥\n";
    
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "❌ Failed to open video file\n";
        return false;
    }
    
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "❌ Failed to find stream info\n";
        avformat_close_input(&fmt_ctx);
        return false;
    }
    
    // Find video stream
    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx == -1) {
            video_stream_idx = i;
        }
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_idx == -1) {
            audio_stream_idx = i;
        }
    }
    
    if (video_stream_idx == -1) {
        std::cerr << "❌ No video stream found\n";
        avformat_close_input(&fmt_ctx);
        return false;
    }
    
    info.has_audio = (audio_stream_idx != -1);
    
    AVStream* video_stream = fmt_ctx->streams[video_stream_idx];
    const AVCodec* video_codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
    
    if (!video_codec) {
        std::cerr << "❌ Video codec not found\n";
        avformat_close_input(&fmt_ctx);
        return false;
    }
    
    AVCodecContext* video_codec_ctx = avcodec_alloc_context3(video_codec);
    avcodec_parameters_to_context(video_codec_ctx, video_stream->codecpar);
    
    if (avcodec_open2(video_codec_ctx, video_codec, nullptr) < 0) {
        std::cerr << "❌ Failed to open video codec\n";
        avcodec_free_context(&video_codec_ctx);
        avformat_close_input(&fmt_ctx);
        return false;
    }
    
    info.width = video_codec_ctx->width;
    info.height = video_codec_ctx->height;
    info.fps_num = video_stream->r_frame_rate.num;
    info.fps_den = video_stream->r_frame_rate.den;
    info.total_frames = video_stream->nb_frames > 0 ? video_stream->nb_frames : 
                       (int)(fmt_ctx->duration * info.fps_num / (info.fps_den * AV_TIME_BASE));
    
    std::cout << "✅ VIDEO: " << info.width << "x" << info.height 
              << " @ " << (info.fps_num / info.fps_den) << " FPS\n";
    std::cout << "📊 Estimated frames: " << info.total_frames << "\n";
    std::cout << "🎵 Audio stream: " << (info.has_audio ? "YES 💚" : "NO") << "\n";
    
    // Setup frame conversion to RGBA
    SwsContext* sws_ctx = sws_getContext(
        info.width, info.height, video_codec_ctx->pix_fmt,
        info.width, info.height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    AVFrame* frame = av_frame_alloc();
    AVFrame* frame_rgba = av_frame_alloc();
    frame_rgba->format = AV_PIX_FMT_RGBA;
    frame_rgba->width = info.width;
    frame_rgba->height = info.height;
    av_frame_get_buffer(frame_rgba, 0);
    
    AVPacket* packet = av_packet_alloc();
    
    std::cout << "🎬 Extracting frames with RGBA...\n";
    
    int frame_count = 0;
    while (av_read_frame(fmt_ctx, packet) >= 0) {
        if (packet->stream_index == video_stream_idx) {
            if (avcodec_send_packet(video_codec_ctx, packet) >= 0) {
                while (avcodec_receive_frame(video_codec_ctx, frame) >= 0) {
                    // Convert to RGBA
                    sws_scale(sws_ctx, frame->data, frame->linesize, 0, info.height,
                            frame_rgba->data, frame_rgba->linesize);
                    
                    // Extract RGBA pixels
                    std::vector<RGBA> pixels(info.width * info.height);
                    uint8_t* rgba_data = frame_rgba->data[0];
                    
                    for (int i = 0; i < info.width * info.height; i++) {
                        pixels[i] = {
                            rgba_data[i * 4],
                            rgba_data[i * 4 + 1],
                            rgba_data[i * 4 + 2],
                            rgba_data[i * 4 + 3]
                        };
                    }
                    
                    frames_data.push_back(pixels);
                    frame_count++;
                    
                    if (frame_count % 30 == 0) {
                        std::cout << "📦 Extracted " << frame_count << " frames...\n";
                    }
                }
            }
        }
        av_packet_unref(packet);
    }
    
    std::cout << "✅ Extracted " << frame_count << " frames total!! 💚\n";
    
// 🎵 EXTRACT AUDIO IF PRESENT - FIXED FOR NEW FFMPEG API!!
if (info.has_audio && audio_out) {
    std::cout << "\n🎵 EXTRACTING AUDIO STREAM...\n";
    
    AVStream* audio_stream = fmt_ctx->streams[audio_stream_idx];
    const AVCodec* audio_codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
    
    if (audio_codec) {
        AVCodecContext* audio_codec_ctx = avcodec_alloc_context3(audio_codec);
        avcodec_parameters_to_context(audio_codec_ctx, audio_stream->codecpar);
        
        if (avcodec_open2(audio_codec_ctx, audio_codec, nullptr) >= 0) {
            audio_out->sample_rate = audio_codec_ctx->sample_rate;
            // FIX: Use ch_layout instead of channels!!
            audio_out->channels = audio_codec_ctx->ch_layout.nb_channels;
            
            std::cout << "✅ AUDIO: " << audio_out->sample_rate << "Hz, " 
                      << audio_out->channels << " channels\n";
            
            // FIX: Setup audio resampler using NEW API!!
            SwrContext* swr_ctx = nullptr;
            AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO;
            if (audio_out->channels == 1) {
                out_ch_layout = AV_CHANNEL_LAYOUT_MONO;
            }
            
            // Use swr_alloc_set_opts2 instead of deprecated function!!
            int ret = swr_alloc_set_opts2(
                &swr_ctx,
                &out_ch_layout,
                AV_SAMPLE_FMT_FLT,
                audio_out->sample_rate,
                &audio_codec_ctx->ch_layout,
                audio_codec_ctx->sample_fmt,
                audio_codec_ctx->sample_rate,
                0, nullptr
            );
            
            if (ret < 0) {
                std::cerr << "❌ Failed to allocate resampler\n";
                avcodec_free_context(&audio_codec_ctx);
                return false;
            }
            
            swr_init(swr_ctx);
            
            AVFrame* audio_frame = av_frame_alloc();
            std::vector<float> interleaved_samples;
            
            // Seek back to start
            av_seek_frame(fmt_ctx, audio_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
            
            while (av_read_frame(fmt_ctx, packet) >= 0) {
                if (packet->stream_index == audio_stream_idx) {
                    if (avcodec_send_packet(audio_codec_ctx, packet) >= 0) {
                        while (avcodec_receive_frame(audio_codec_ctx, audio_frame) >= 0) {
                            uint8_t* out_buffer = nullptr;
                            int out_samples = av_rescale_rnd(
                                swr_get_delay(swr_ctx, audio_out->sample_rate) + audio_frame->nb_samples,
                                audio_out->sample_rate, audio_out->sample_rate, AV_ROUND_UP
                            );
                            
                            av_samples_alloc(&out_buffer, nullptr, audio_out->channels,
                                           out_samples, AV_SAMPLE_FMT_FLT, 0);
                            
                            out_samples = swr_convert(swr_ctx, &out_buffer, out_samples,
                                                    (const uint8_t**)audio_frame->data,
                                                    audio_frame->nb_samples);
                            
                            float* float_buffer = (float*)out_buffer;
                            for (int i = 0; i < out_samples * audio_out->channels; i++) {
                                interleaved_samples.push_back(float_buffer[i]);
                            }
                            
                            av_freep(&out_buffer);
                        }
                    }
                }
                av_packet_unref(packet);
            }
            
            audio_out->total_samples = interleaved_samples.size() / audio_out->channels;
            
            // De-interleave
            audio_out->channel_data.resize(audio_out->channels);
            for (int ch = 0; ch < audio_out->channels; ch++) {
                audio_out->channel_data[ch].resize(audio_out->total_samples);
                for (int64_t i = 0; i < audio_out->total_samples; i++) {
                    audio_out->channel_data[ch][i] = interleaved_samples[i * audio_out->channels + ch];
                }
            }
            
            std::cout << "✅ Extracted " << audio_out->total_samples << " audio samples!! 💚\n";
            
            av_frame_free(&audio_frame);
            swr_free(&swr_ctx);
            avcodec_free_context(&audio_codec_ctx);
        }
    }
}
    
    // Cleanup
    av_frame_free(&frame);
    av_frame_free(&frame_rgba);
    av_packet_free(&packet);
    sws_freeContext(sws_ctx);
    avcodec_free_context(&video_codec_ctx);
    avformat_close_input(&fmt_ctx);
    
    return true;
}

// 🌐 WEBP IMAGE LOADER
bool load_webp_image(const std::string& path, int& w, int& h, std::vector<RGBA>& pixels) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;
    
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    if (!file.read((char*)buffer.data(), size)) return false;
    file.close();
    
    uint8_t* decoded = WebPDecodeRGBA(buffer.data(), buffer.size(), &w, &h);
    if (!decoded) return false;
    
    pixels.resize(w * h);
    for (int i = 0; i < w * h; i++) {
        pixels[i] = {decoded[i*4], decoded[i*4+1], decoded[i*4+2], decoded[i*4+3]};
    }
    
    WebPFree(decoded);
    return true;
}

// 🎨 UNIVERSAL IMAGE LOADER
bool load_universal_image(const std::string& path, int& w, int& h, std::vector<RGBA>& pixels) {
    std::string ext = get_file_extension(path);
    
    if (ext == "webp") {
        return load_webp_image(path, w, h, pixels);
    }
    
    int channels;
    unsigned char* img_data = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!img_data) return false;
    
    pixels.resize(w * h);
    for (int i = 0; i < w * h; i++) {
        pixels[i] = {img_data[i*4], img_data[i*4+1], img_data[i*4+2], img_data[i*4+3]};
    }
    
    stbi_image_free(img_data);
    return true;
}

// 🎬 GIF LOADER
bool load_gif_frames(const std::string& path, int& w, int& h, int& n_frames, int& fps,
                    std::vector<std::vector<RGBA>>& frames_data) {
    
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;
    
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<unsigned char> buffer(size);
    if (!file.read((char*)buffer.data(), size)) return false;
    file.close();
    
    int channels, z = 0;
    int* delays = nullptr;
    unsigned char* img_data = stbi_load_gif_from_memory(buffer.data(), buffer.size(), 
                                                        &delays, &w, &h, &z, &channels, 4);
    if (!img_data) return false;
    
    n_frames = z;
    fps = (delays && delays[0] > 0) ? std::max(1, 1000 / delays[0]) : 10;
    
    for (int frame_idx = 0; frame_idx < n_frames; frame_idx++) {
        std::vector<RGBA> frame_pixels(w * h);
        int offset = frame_idx * w * h * 4;
        
        for (int i = 0; i < w * h; i++) {
            frame_pixels[i] = {
                img_data[offset + i*4],
                img_data[offset + i*4+1],
                img_data[offset + i*4+2],
                img_data[offset + i*4+3]
            };
        }
        frames_data.push_back(frame_pixels);
    }
    
    stbi_image_free(img_data);
    if (delays) free(delays);
    
    return true;
}

// 🎯 RLE COMPRESSION FOR AUDIO
std::string compress_channel_data(const std::vector<float>& samples, float epsilon = 0.00001f) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(6);
    
    int64_t i = 0, total = samples.size();
    
    while (i < total) {
        float value = samples[i];
        int64_t run_length = 1;
        
        while (i + run_length < total && std::abs(samples[i + run_length] - value) < epsilon) {
            run_length++;
        }
        
        if (run_length >= 5) {
            ss << i << "-" << (i + run_length - 1) << "=" << value;
            if (i + run_length < total) ss << ",";
        } else {
            for (int64_t j = 0; j < run_length; j++) {
                ss << samples[i + j];
                if (i + j < total - 1) ss << ",";
            }
        }
        
        i += run_length;
    }
    
    return ss.str();
}

// 💾 BUILD HMICA FORMAT
std::string build_hmica_data(const AudioData& audio) {
    std::stringstream data;
    data << "info{\nhz=" << audio.sample_rate << "\nc=" << audio.channels 
         << "\nsam=" << audio.total_samples << "\n}\n\n";
    
    for (int ch = 0; ch < audio.channels; ch++) {
        data << "C" << (ch + 1) << "{\n";
        data << compress_channel_data(audio.channel_data[ch]);
        data << "\n}\n";
        if (ch < audio.channels - 1) data << "\n";
    }
    
    return data.str();
}

// 🚀 PROCESS FRAME ROWS
void process_frame_rows_parallel(
    const std::vector<RGBA>& frame_pixels, int w, int h,
    int start_row, int end_row,
    std::map<RGBA, std::vector<Command>>* local_commands
) {
    for (int y = start_row; y < end_row; y++) {
        int x = 0;
        while (x < w) {
            RGBA pixel_color = frame_pixels[y * w + x];
            int run_length = 1;
            
            while (x + run_length < w && frame_pixels[y * w + x + run_length] == pixel_color) {
                run_length++;
            }
            
            int end_x = x + run_length - 1;
            std::string cmd = (run_length == 1) ?
                "P=" + std::to_string(x+1) + "x" + std::to_string(y+1) :
                "PL=" + std::to_string(x+1) + "x" + std::to_string(y+1) + "-" + 
                std::to_string(end_x+1) + "x" + std::to_string(y+1);
            
            (*local_commands)[pixel_color].push_back({cmd, x, end_x, y});
            x += run_length;
        }
    }
}

// 💾 BUILD HMIC FORMAT
std::string build_hmic_data(int w, int h, int fps, int n_frames,
                           std::vector<std::map<RGBA, std::vector<Command>>>& frame_commands) {
    
    int num_threads = std::thread::hardware_concurrency();
    std::vector<std::set<Command>> merged_commands(n_frames);
    std::map<std::string, std::map<RGBA, std::vector<std::string>>> temporal_commands;
    
    std::cout << "\n🚀 Temporal optimization...\n";
    
    for (int frame_idx = 0; frame_idx < n_frames - 1; frame_idx++) {
        for (const auto& [color, cmd_list] : frame_commands[frame_idx]) {
            for (const auto& cmd_data : cmd_list) {
                if (merged_commands[frame_idx].count(cmd_data)) continue;
                
                std::vector<int> consecutive_frames = {frame_idx + 1};
                
                for (int next_frame_idx = frame_idx + 1; next_frame_idx < n_frames; next_frame_idx++) {
                    bool match_found = false;
                    
                    if (frame_commands[next_frame_idx].count(color)) {
                        for (const auto& next_cmd_data : frame_commands[next_frame_idx][color]) {
                            if (next_cmd_data.x == cmd_data.x && 
                                next_cmd_data.end_x == cmd_data.end_x && 
                                next_cmd_data.y == cmd_data.y) {
                                
                                if (!merged_commands[next_frame_idx].count(next_cmd_data)) {
                                    consecutive_frames.push_back(next_frame_idx + 1);
                                    merged_commands[next_frame_idx].insert(next_cmd_data);
                                    match_found = true;
                                    break;
                                }
                            }
                        }
                    }
                    
                    if (!match_found) break;
                }
                
                if (consecutive_frames.size() > 1) {
                    std::string frame_range_str = frames_to_range_string(consecutive_frames);
                    temporal_commands[frame_range_str][color].push_back(cmd_data.cmd);
                    merged_commands[frame_idx].insert(cmd_data);
                }
            }
        }
    }
    
    std::stringstream data;
    data << "info{\nDISPLAY=" << w << "X" << h << "\nFPS=" << fps 
         << "\nF=" << n_frames << "\nLOOP=Y\n}\n\n";
    
    for (const auto& [frame_range_str, color_commands] : temporal_commands) {
        data << "F" << frame_range_str << "{\n";
        for (const auto& [color, cmds] : color_commands) {
            data << "  rgba(" << (int)color.r << "," << (int)color.g << "," 
                 << (int)color.b << "," << (int)color.a << "){\n";
            for (const auto& cmd : cmds) {
                data << "    " << cmd << "\n";
            }
            data << "  }\n";
        }
        data << "}\n";
    }
    
    for (int frame_idx = 0; frame_idx < n_frames; frame_idx++) {
        std::stringstream frame_data;
        frame_data << "F" << (frame_idx + 1) << "{\n";
        bool has_content = false;
        
        for (const auto& [color, cmd_list] : frame_commands[frame_idx]) {
            bool color_written = false;
            
            for (const auto& cmd_data : cmd_list) {
                if (!merged_commands[frame_idx].count(cmd_data)) {
                    if (!color_written) {
                        has_content = true;
                        frame_data << "  rgba(" << (int)color.r << "," << (int)color.g << "," 
                                   << (int)color.b << "," << (int)color.a << "){\n";
                        color_written = true;
                    }
                    frame_data << "    " << cmd_data.cmd << "\n";
                }
            }
            
            if (color_written) frame_data << "  }\n";
        }
        
        frame_data << "}\n";
        if (has_content) data << frame_data.str();
    }
    
    return data.str();
}

int main() {
    mpg123_init();
    
    std::cout << "🔥🔥🔥 HMIC-A UNIVERSAL MEDIA CONVERTER 🔥🔥🔥\n";
    std::cout << "🎬 VIDEO: MP4, AVI, MOV, WEBM, MKV, FLV + MORE!!\n";
    std::cout << "🎨 IMAGE: JPG, PNG, BMP, GIF, WEBP, APNG, TGA!!\n";
    std::cout << "🎵 AUDIO: Automatically extracted from videos!!\n";
    std::cout << "💎 OUTPUT: HMIC (visual) + HMICA (audio) + COMBINED FORMAT!!\n\n";
    
    std::string media_path;
    std::cout << "Enter media file path (video/image): ";
    std::getline(std::cin, media_path);
    
    if (!fs::exists(media_path)) {
        std::cerr << "❌ File not found\n";
        mpg123_exit();
        return 1;
    }
    
    std::string ext = get_file_extension(media_path);
    bool is_video = (ext == "mp4" || ext == "avi" || ext == "mov" || ext == "webm" || 
                     ext == "mkv" || ext == "flv" || ext == "wmv" || ext == "m4v");
    bool is_gif = (ext == "gif");
    
    int w, h, n_frames = 1, fps = 1;
    std::vector<std::vector<RGBA>> frames_data;
    AudioData audio;
    bool has_audio = false;
    
    if (is_video) {
        std::cout << "\n🎬 VIDEO MODE!! Extracting frames + audio...\n";
        VideoInfo info;
        
        if (!extract_video_frames(media_path, info, frames_data, &audio)) {
            mpg123_exit();
            return 1;
        }
        
        w = info.width;
        h = info.height;
        n_frames = frames_data.size();
        fps = info.fps_num / info.fps_den;
        has_audio = info.has_audio && audio.total_samples > 0;
        
    } else if (is_gif) {
        std::cout << "\n🎬 GIF MODE!! Extracting animated frames...\n";
        
        if (!load_gif_frames(media_path, w, h, n_frames, fps, frames_data)) {
            mpg123_exit();
            return 1;
        }
        
        std::cout << "✅ GIF loaded: " << n_frames << " frames @ " << fps << " FPS\n";
        
    } else {
        std::cout << "\n📸 STATIC IMAGE MODE!!\n";
        
        std::vector<RGBA> pixels;
        if (!load_universal_image(media_path, w, h, pixels)) {
            mpg123_exit();
            return 1;
        }
        
        frames_data.push_back(pixels);
        std::cout << "✅ Image loaded: " << w << "x" << h << "\n";
    }
    
    // Get output format
    std::string mode;
    std::cout << "\nChoose compression (NONE / ZSTD): ";
    std::getline(std::cin, mode);
    std::transform(mode.begin(), mode.end(), mode.begin(), ::toupper);
    bool compress = (mode == "ZSTD");
    
    // 🧠 PROCESS ALL FRAMES
    std::cout << "\n🎨 Processing " << n_frames << " frames with " 
              << std::thread::hardware_concurrency() << " threads...\n";
    
    std::vector<std::map<RGBA, std::vector<Command>>> frame_commands(n_frames);
    int num_threads = std::thread::hardware_concurrency();
    
    for (int frame_idx = 0; frame_idx < n_frames; frame_idx++) {
        processed_rows = 0;
        
        int rows_per_chunk = std::max(1, h / num_threads);
        std::vector<std::thread> threads;
        std::vector<std::map<RGBA, std::vector<Command>>> thread_results(num_threads);
        
        for (int t = 0; t < num_threads; t++) {
            int start_row = t * rows_per_chunk;
            int end_row = (t == num_threads - 1) ? h : (t + 1) * rows_per_chunk;
            
            threads.emplace_back(process_frame_rows_parallel,
                               std::ref(frames_data[frame_idx]), w, h,
                               start_row, end_row, &thread_results[t]);
        }
        
        for (auto& t : threads) t.join();
        
        // Merge results
        for (const auto& result : thread_results) {
            for (const auto& [color, cmds] : result) {
                frame_commands[frame_idx][color].insert(
                    frame_commands[frame_idx][color].end(),
                    cmds.begin(), cmds.end()
                );
            }
        }
        
        processed_frames++;
        std::cout << "✅ Frame " << processed_frames << "/" << n_frames << " processed\n";
    }
    
    // 💾 BUILD HMIC DATA
    std::cout << "\n📝 Building HMIC visual data...\n";
    std::string hmic_text = build_hmic_data(w, h, fps, n_frames, frame_commands);
    
    std::string base_name = fs::path(media_path).stem().string();
    
    // 🎵 BUILD HMICA DATA IF AUDIO EXISTS
    std::string hmica_text;
    if (has_audio) {
        std::cout << "📝 Building HMICA audio data...\n";
        hmica_text = build_hmica_data(audio);
    }
    
    // 🚀 WRITE OUTPUT FILES
    std::cout << "\n💾 Writing output files...\n";
    
    std::string hmic_file = base_name + (compress ? ".hmic7" : ".hmic");
    std::string hmica_file = base_name + (compress ? ".hmica7" : ".hmica");
    std::string combined_file = base_name + (compress ? ".hmicav7" : ".hmicav");
    
    if (compress) {
        // Compress HMIC
        size_t hmic_bound = ZSTD_compressBound(hmic_text.size());
        std::vector<char> hmic_compressed(hmic_bound);
        size_t hmic_size = ZSTD_compress(hmic_compressed.data(), hmic_bound,
                                         hmic_text.c_str(), hmic_text.size(), 19);
        
        if (!ZSTD_isError(hmic_size)) {
            std::ofstream file(hmic_file, std::ios::binary);
            file.write(hmic_compressed.data(), hmic_size);
            file.close();
            std::cout << "✅ " << hmic_file << " created (" << (hmic_size / 1024.0) << " KB)\n";
        }
        
        // Compress HMICA if exists
        if (has_audio) {
            size_t hmica_bound = ZSTD_compressBound(hmica_text.size());
            std::vector<char> hmica_compressed(hmica_bound);
            size_t hmica_size = ZSTD_compress(hmica_compressed.data(), hmica_bound,
                                             hmica_text.c_str(), hmica_text.size(), 19);
            
            if (!ZSTD_isError(hmica_size)) {
                std::ofstream file(hmica_file, std::ios::binary);
                file.write(hmica_compressed.data(), hmica_size);
                file.close();
                std::cout << "✅ " << hmica_file << " created (" << (hmica_size / 1024.0) << " KB)\n";
            }
        }
        
        // Create combined format
        std::stringstream combined;
        combined << "HMICAV_HEADER{\n";
        combined << "VERSION=1.0\n";
        combined << "HAS_VIDEO=Y\n";
        combined << "HAS_AUDIO=" << (has_audio ? "Y" : "N") << "\n";
        combined << "VIDEO_SIZE=" << hmic_text.size() << "\n";
        if (has_audio) combined << "AUDIO_SIZE=" << hmica_text.size() << "\n";
        combined << "}\n\n";
        combined << "VIDEO_DATA{\n" << hmic_text << "\n}\n";
        if (has_audio) combined << "\nAUDIO_DATA{\n" << hmica_text << "\n}\n";
        
        std::string combined_text = combined.str();
        size_t combined_bound = ZSTD_compressBound(combined_text.size());
        std::vector<char> combined_compressed(combined_bound);
        size_t combined_size = ZSTD_compress(combined_compressed.data(), combined_bound,
                                             combined_text.c_str(), combined_text.size(), 19);
        
        if (!ZSTD_isError(combined_size)) {
            std::ofstream file(combined_file, std::ios::binary);
            file.write(combined_compressed.data(), combined_size);
            file.close();
            std::cout << "✅ " << combined_file << " created (" << (combined_size / 1024.0) << " KB)\n";
        }
        
    } else {
        // Write uncompressed
        std::ofstream hmic_out(hmic_file);
        hmic_out << hmic_text;
        hmic_out.close();
        std::cout << "✅ " << hmic_file << " created (" << (hmic_text.size() / 1024.0) << " KB)\n";
        
        if (has_audio) {
            std::ofstream hmica_out(hmica_file);
            hmica_out << hmica_text;
            hmica_out.close();
            std::cout << "✅ " << hmica_file << " created (" << (hmica_text.size() / 1024.0) << " KB)\n";
        }
        
        // Create combined format
        std::ofstream combined_out(combined_file);
        combined_out << "HMICAV_HEADER{\n";
        combined_out << "VERSION=1.0\n";
        combined_out << "HAS_VIDEO=Y\n";
        combined_out << "HAS_AUDIO=" << (has_audio ? "Y" : "N") << "\n";
        combined_out << "VIDEO_SIZE=" << hmic_text.size() << "\n";
        if (has_audio) combined_out << "AUDIO_SIZE=" << hmica_text.size() << "\n";
        combined_out << "}\n\n";
        combined_out << "VIDEO_DATA{\n" << hmic_text << "\n}\n";
        if (has_audio) combined_out << "\nAUDIO_DATA{\n" << hmica_text << "\n}\n";
        combined_out.close();
        std::cout << "✅ " << combined_file << " created\n";
    }
    
    // 📊 FINAL STATS
    std::cout << "\n📊 ═══════════ FINAL STATS ═══════════ 📊\n";
    std::cout << "📁 Input: ." << ext << " (" << (is_video ? "VIDEO" : (is_gif ? "GIF" : "IMAGE")) << ")\n";
    std::cout << "📺 Resolution: " << w << "x" << h << "\n";
    std::cout << "🎬 Frames: " << n_frames << " @ " << fps << " FPS\n";
    if (has_audio) {
        std::cout << "🎵 Audio: " << audio.sample_rate << "Hz, " << audio.channels 
                  << " channels, " << audio.total_samples << " samples\n";
        std::cout << "⏱️  Audio duration: " << (float)audio.total_samples / audio.sample_rate << "s\n";
    } else {
        std::cout << "🎵 Audio: None\n";
    }
    std::cout << "💾 Compression: " << (compress ? "Zstd level 19" : "None") << "\n";
    std::cout << "🧵 Threads used: " << num_threads << "\n";
    
    std::cout << "\n💥 CONVERSION COMPLETE!! 💥\n";
    std::cout << "📦 Files created:\n";
    std::cout << "   - " << hmic_file << " (visual data)\n";
    if (has_audio) std::cout << "   - " << hmica_file << " (audio data)\n";
    std::cout << "   - " << combined_file << " (combined format)\n";
    std::cout << "\n🔥 THE FUTURE OF MEDIA IS HERE!! 🔥\n";
    std::cout << "✨ FULL RGBA + TEMPORAL COMPRESSION + MULTI-THREADED ✨\n";
    
    mpg123_exit();
    return 0;
}