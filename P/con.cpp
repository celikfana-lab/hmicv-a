#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <cstring>
#include <algorithm>

// 🎬 VIDEO DECODING - FFMPEG LIBRARIES
extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
    #include <libswscale/swscale.h>
    #include <libswresample/swresample.h>
}

// 🎨 IMAGE DECODING
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// 🌐 WEBP SUPPORT
#include <webp/decode.h>

// 🚀 COMPRESSION
#include <zstd.h>

namespace fs = std::filesystem;

// 🎨 RGBA STRUCT
struct RGBA {
    uint8_t r, g, b, a;
};

// 🎬 VIDEO INFO
struct VideoInfo {
    int width, height;
    int fps_num, fps_den;
    int total_frames;
    bool has_audio;
};

// 🎧 AUDIO DATA
struct AudioData {
    int sample_rate;
    int channels;
    int64_t total_samples;
    std::vector<std::vector<float>> channel_data;
};

// ⚡ HMIC-FAST BINARY FORMAT HEADER
#pragma pack(push, 1)
struct HMICFastHeader {
    char magic[8];           // "HMICFAST"
    uint32_t version;        // Format version (1)
    uint32_t width;          // Frame width
    uint32_t height;         // Frame height
    uint32_t fps;            // Frames per second
    uint32_t total_frames;   // Total number of frames
    uint8_t has_audio;       // 1 if audio present, 0 if not
    uint8_t compressed;      // 1 if frames are zstd compressed individually
    uint32_t audio_sample_rate;
    uint8_t audio_channels;
    uint64_t audio_samples;
    uint64_t frame_index_offset;  // Offset to frame index table
    uint64_t audio_data_offset;   // Offset to audio data
};

struct FrameIndexEntry {
    uint64_t offset;         // Byte offset in file
    uint32_t size;           // Compressed or uncompressed size
};
#pragma pack(pop)

// 🔥 FILE EXTENSION DETECTOR
std::string get_file_extension(const std::string& path) {
    size_t dot_pos = path.find_last_of('.');
    if (dot_pos == std::string::npos) return "";
    std::string ext = path.substr(dot_pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
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
    
    // 🎵 EXTRACT AUDIO IF PRESENT
    if (info.has_audio && audio_out) {
        std::cout << "\n🎵 EXTRACTING AUDIO STREAM...\n";
        
        AVStream* audio_stream = fmt_ctx->streams[audio_stream_idx];
        const AVCodec* audio_codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
        
        if (audio_codec) {
            AVCodecContext* audio_codec_ctx = avcodec_alloc_context3(audio_codec);
            avcodec_parameters_to_context(audio_codec_ctx, audio_stream->codecpar);
            
            if (avcodec_open2(audio_codec_ctx, audio_codec, nullptr) >= 0) {
                audio_out->sample_rate = audio_codec_ctx->sample_rate;
                audio_out->channels = audio_codec_ctx->ch_layout.nb_channels;
                
                std::cout << "✅ AUDIO: " << audio_out->sample_rate << "Hz, " 
                          << audio_out->channels << " channels\n";
                
                SwrContext* swr_ctx = nullptr;
                AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO;
                if (audio_out->channels == 1) {
                    out_ch_layout = AV_CHANNEL_LAYOUT_MONO;
                }
                
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

// ⚡⚡⚡ WRITE HMIC-FAST BINARY FORMAT ⚡⚡⚡
bool write_hmicfast_binary(const std::string& output_path,
                          int w, int h, int fps,
                          const std::vector<std::vector<RGBA>>& frames_data,
                          const AudioData* audio,
                          bool compress_frames) {
    
    std::cout << "\n⚡⚡⚡ WRITING HMIC-FAST BINARY FORMAT ⚡⚡⚡\n";
    std::cout << "🔥 THIS WILL BE ULTRA FAST TO LOAD!! NO PARSING!! 🔥\n";
    
    std::ofstream file(output_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "❌ Failed to create output file\n";
        return false;
    }
    
    // Write header
    HMICFastHeader header = {};
    memcpy(header.magic, "HMICFAST", 8);
    header.version = 1;
    header.width = w;
    header.height = h;
    header.fps = fps;
    header.total_frames = frames_data.size();
    header.has_audio = (audio && audio->total_samples > 0) ? 1 : 0;
    header.compressed = compress_frames ? 1 : 0;
    
    if (header.has_audio) {
        header.audio_sample_rate = audio->sample_rate;
        header.audio_channels = audio->channels;
        header.audio_samples = audio->total_samples;
    }
    
    file.write((char*)&header, sizeof(HMICFastHeader));
    
    // Reserve space for frame index
    uint64_t frame_index_pos = file.tellp();
    std::vector<FrameIndexEntry> frame_index(frames_data.size());
    file.write((char*)frame_index.data(), sizeof(FrameIndexEntry) * frames_data.size());
    
    // Write frames
    std::cout << "📦 Writing " << frames_data.size() << " frames...\n";
    
    for (size_t i = 0; i < frames_data.size(); i++) {
        frame_index[i].offset = file.tellp();
        
        const auto& frame = frames_data[i];
        size_t frame_size = frame.size() * sizeof(RGBA);
        
        if (compress_frames) {
            // Compress with Zstd
            size_t compress_bound = ZSTD_compressBound(frame_size);
            std::vector<char> compressed(compress_bound);
            size_t compressed_size = ZSTD_compress(compressed.data(), compress_bound,
                                                   frame.data(), frame_size, 3); // Level 3 for speed
            
            if (!ZSTD_isError(compressed_size)) {
                frame_index[i].size = compressed_size;
                file.write(compressed.data(), compressed_size);
            } else {
                std::cerr << "❌ Compression failed for frame " << i << "\n";
                return false;
            }
        } else {
            // Write raw frame data
            frame_index[i].size = frame_size;
            file.write((char*)frame.data(), frame_size);
        }
        
        if ((i + 1) % 30 == 0) {
            std::cout << "✅ Written " << (i + 1) << "/" << frames_data.size() << " frames\n";
        }
    }
    
    // Write audio if present
    if (header.has_audio) {
        header.audio_data_offset = file.tellp();
        std::cout << "\n🎵 Writing audio data...\n";
        
        // Write interleaved float audio
        for (int64_t i = 0; i < audio->total_samples; i++) {
            for (int ch = 0; ch < audio->channels; ch++) {
                float sample = audio->channel_data[ch][i];
                file.write((char*)&sample, sizeof(float));
            }
        }
        
        std::cout << "✅ Audio written: " << audio->total_samples << " samples\n";
    }
    
    // Update header with offsets
    header.frame_index_offset = frame_index_pos;
    file.seekp(0);
    file.write((char*)&header, sizeof(HMICFastHeader));
    
    // Write updated frame index
    file.seekp(frame_index_pos);
    file.write((char*)frame_index.data(), sizeof(FrameIndexEntry) * frames_data.size());
    
    file.close();
    
    std::cout << "\n💚 HMIC-FAST BINARY CREATED!! 💚\n";
    std::cout << "⚡ PLAYER CAN NOW MEMMAP AND INSTANT LOAD!! ⚡\n";
    
    return true;
}

int main() {
    std::cout << "⚡⚡⚡ HMIC-FAST ULTRA SPEED BINARY CONVERTER ⚡⚡⚡\n";
    std::cout << "🔥 PRE-RENDERED BINARY FORMAT FOR INSTANT PLAYBACK!! 🔥\n";
    std::cout << "🎬 VIDEO: MP4, AVI, MOV, WEBM, MKV + MORE!!\n";
    std::cout << "🎨 IMAGE: JPG, PNG, BMP, GIF, WEBP!!\n";
    std::cout << "💾 OUTPUT: PURE BINARY - NO PARSING NEEDED!!\n\n";
    
    std::string media_path;
    std::cout << "Enter media file path: ";
    std::getline(std::cin, media_path);
    
    if (!fs::exists(media_path)) {
        std::cerr << "❌ File not found\n";
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
            return 1;
        }
        
        std::cout << "✅ GIF loaded: " << n_frames << " frames @ " << fps << " FPS\n";
        
    } else {
        std::cout << "\n📸 STATIC IMAGE MODE!!\n";
        
        std::vector<RGBA> pixels;
        if (!load_universal_image(media_path, w, h, pixels)) {
            return 1;
        }
        
        frames_data.push_back(pixels);
        std::cout << "✅ Image loaded: " << w << "x" << h << "\n";
    }
    
    // Ask about frame compression
    std::string compress_choice;
    std::cout << "\nCompress frames? (Y/N - recommended Y for disk, N for max speed): ";
    std::getline(std::cin, compress_choice);
    std::transform(compress_choice.begin(), compress_choice.end(), compress_choice.begin(), ::toupper);
    bool compress_frames = (compress_choice == "Y" || compress_choice == "YES");
    
    std::string base_name = fs::path(media_path).stem().string();
    std::string output_file = base_name + ".hmicfast";
    
    // Write the binary format
    if (!write_hmicfast_binary(output_file, w, h, fps, frames_data, 
                              has_audio ? &audio : nullptr, compress_frames)) {
        return 1;
    }
    
    // Get file size
    auto file_size = fs::file_size(output_file);
    
    std::cout << "\n📊 ═══════════ FINAL STATS ═══════════ 📊\n";
    std::cout << "📁 Input: ." << ext << " (" << (is_video ? "VIDEO" : (is_gif ? "GIF" : "IMAGE")) << ")\n";
    std::cout << "📺 Resolution: " << w << "x" << h << "\n";
    std::cout << "🎬 Frames: " << n_frames << " @ " << fps << " FPS\n";
    if (has_audio) {
        std::cout << "🎵 Audio: " << audio.sample_rate << "Hz, " << audio.channels 
                  << " channels, " << audio.total_samples << " samples\n";
    }
    std::cout << "💾 Frame compression: " << (compress_frames ? "Zstd level 3" : "None (RAW)") << "\n";
    std::cout << "📦 Output size: " << (file_size / 1024.0 / 1024.0) << " MB\n";
    std::cout << "\n💥 CONVERSION COMPLETE!! 💥\n";
    std::cout << "⚡ File: " << output_file << "\n";
    std::cout << "\n🚀 HOW TO USE IN PLAYER:\n";
    std::cout << "1. Memory-map the file for INSTANT loading\n";
    std::cout << "2. Read header to get dimensions/fps\n";
    std::cout << "3. Jump directly to any frame using the index\n";
    std::cout << "4. Decompress on-the-fly if compressed (still fast!)\n";
    std::cout << "5. Memcpy directly to GPU/screen buffer - NO PARSING!! ⚡⚡⚡\n";
    std::cout << "\n🔥 THIS IS THE FUTURE!! SPEED MODE ACTIVATED!! 🔥\n";
    
    return 0;
}