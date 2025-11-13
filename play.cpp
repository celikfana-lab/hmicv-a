#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <map>
#include <sstream>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cmath>
#include <queue>

// 🎮 SDL2 FOR RENDERING + AUDIO
#include <SDL2/SDL.h>

// 🚀 ZSTD DECOMPRESSION
#include <zstd.h>

// 🎨 RGBA COLOR STRUCT
struct RGBA {
    uint8_t r, g, b, a;
};

// 🎬 FRAME DATA
struct Frame {
    int frame_number;
    std::map<RGBA, std::vector<std::string>> commands;
};

// 🎵 AUDIO DATA
struct AudioData {
    int sample_rate;
    int channels;
    int64_t total_samples;
    std::vector<std::vector<float>> channel_data;
};

// 📺 VIDEO INFO
struct VideoInfo {
    int width, height;
    int fps;
    int total_frames;
    bool loop;
};

// 🎮 PLAYER STATE - FIXED FOR SYNC!!
struct PlayerState {
    bool playing = false;
    bool has_audio = false;
    int current_frame = 0;
    std::atomic<int64_t> target_audio_sample{0};  // TARGET sample based on frame
    int64_t audio_sample_pos = 0;  // Actual audio position
    std::chrono::high_resolution_clock::time_point start_time;
    double frame_duration_ms = 0;
    double samples_per_frame = 0;  // NEW!! Audio samples per video frame
    std::atomic<bool> quit{false};
    std::atomic<bool> seeking{false};  // NEW!! Seeking flag
    SDL_AudioDeviceID audio_device = 0;
};

std::mutex audio_mutex;
PlayerState player_state;
AudioData audio_data;
VideoInfo video_info;
std::vector<Frame> frames;
SDL_Surface* screen_surface = nullptr;

// 🎨 RGBA COMPARISON FOR MAP
bool operator<(const RGBA& a, const RGBA& b) {
    if (a.r != b.r) return a.r < b.r;
    if (a.g != b.g) return a.g < b.g;
    if (a.b != b.b) return a.b < b.b;
    return a.a < b.a;
}

// 🔥 DECOMPRESS ZSTD
std::string decompress_zstd(const std::vector<char>& compressed) {
    size_t decompressed_size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    
    if (decompressed_size == ZSTD_CONTENTSIZE_ERROR || 
        decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        std::cerr << "❌ Cannot determine decompressed size\n";
        return "";
    }
    
    std::string decompressed(decompressed_size, '\0');
    size_t result = ZSTD_decompress(&decompressed[0], decompressed_size,
                                    compressed.data(), compressed.size());
    
    if (ZSTD_isError(result)) {
        std::cerr << "❌ Decompression error: " << ZSTD_getErrorName(result) << "\n";
        return "";
    }
    
    return decompressed;
}

// 📖 PARSE RGBA COLOR
RGBA parse_rgba(const std::string& color_str) {
    RGBA color = {0, 0, 0, 255};
    
    size_t start = color_str.find('(');
    size_t end = color_str.find(')');
    if (start == std::string::npos || end == std::string::npos) return color;
    
    std::string values = color_str.substr(start + 1, end - start - 1);
    std::stringstream ss(values);
    std::string token;
    int idx = 0;
    
    while (std::getline(ss, token, ',') && idx < 4) {
        int val = std::stoi(token);
        if (idx == 0) color.r = val;
        else if (idx == 1) color.g = val;
        else if (idx == 2) color.b = val;
        else if (idx == 3) color.a = val;
        idx++;
    }
    
    return color;
}

// 📖 PARSE FRAME RANGE
std::vector<int> parse_frame_range(const std::string& range_str) {
    std::vector<int> frames;
    std::stringstream ss(range_str);
    std::string token;
    
    while (std::getline(ss, token, ',')) {
        size_t dash = token.find('-');
        if (dash != std::string::npos) {
            int start = std::stoi(token.substr(0, dash));
            int end = std::stoi(token.substr(dash + 1));
            for (int i = start; i <= end; i++) {
                frames.push_back(i);
            }
        } else {
            frames.push_back(std::stoi(token));
        }
    }
    
    return frames;
}

// 📖 PARSE AUDIO CHANNEL DATA
bool parse_audio_channel(const std::string& data, std::vector<float>& samples) {
    std::stringstream ss(data);
    std::string token;
    
    while (std::getline(ss, token, ',')) {
        size_t dash = token.find('-');
        size_t eq = token.find('=');
        
        if (dash != std::string::npos && eq != std::string::npos) {
            // RLE format: start-end=value
            int start = std::stoi(token.substr(0, dash));
            int end = std::stoi(token.substr(dash + 1, eq - dash - 1));
            float value = std::stof(token.substr(eq + 1));
            
            while (samples.size() <= end) {
                samples.push_back(0.0f);
            }
            
            for (int i = start; i <= end; i++) {
                samples[i] = value;
            }
        } else {
            // Single value
            samples.push_back(std::stof(token));
        }
    }
    
    return true;
}

// 📖 PARSE HMICAV FILE
bool parse_hmicav(const std::string& content) {
    std::cout << "📖 Parsing HMICAV data...\n";
    
    std::stringstream ss(content);
    std::string line;
    
    enum ParseState { HEADER, VIDEO_INFO, VIDEO_FRAMES, AUDIO_INFO, AUDIO_CHANNELS, NONE };
    ParseState state = NONE;
    
    std::string current_frame_range;
    RGBA current_color;
    int current_audio_channel = 0;
    
    while (std::getline(ss, line)) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        if (line.empty()) continue;
        
        if (line.find("HMICAV_HEADER{") != std::string::npos || 
            line.find("VIDEO_DATA{") != std::string::npos) {
            continue;
        }
        
        if (line.find("info{") != std::string::npos) {
            if (state == NONE || state == HEADER) {
                state = VIDEO_INFO;
            } else {
                state = AUDIO_INFO;
            }
            continue;
        }
        
        if (line.find("AUDIO_DATA{") != std::string::npos) {
            state = AUDIO_INFO;
            continue;
        }
        
        if (line == "}") {
            if (state == VIDEO_INFO) state = VIDEO_FRAMES;
            else if (state == AUDIO_INFO) state = AUDIO_CHANNELS;
            continue;
        }
        
        if (state == VIDEO_INFO) {
            if (line.find("DISPLAY=") == 0) {
                std::string res = line.substr(8);
                size_t x_pos = res.find('X');
                video_info.width = std::stoi(res.substr(0, x_pos));
                video_info.height = std::stoi(res.substr(x_pos + 1));
                std::cout << "📺 Resolution: " << video_info.width << "x" << video_info.height << "\n";
            }
            else if (line.find("FPS=") == 0) {
                video_info.fps = std::stoi(line.substr(4));
                player_state.frame_duration_ms = 1000.0 / video_info.fps;
                std::cout << "🎬 FPS: " << video_info.fps << "\n";
            }
            else if (line.find("F=") == 0) {
                video_info.total_frames = std::stoi(line.substr(2));
                frames.resize(video_info.total_frames);
                std::cout << "📊 Total frames: " << video_info.total_frames << "\n";
            }
            else if (line.find("LOOP=") == 0) {
                video_info.loop = (line.substr(5) == "Y");
                std::cout << "🔁 Loop: " << (video_info.loop ? "YES" : "NO") << "\n";
            }
        }
        else if (state == VIDEO_FRAMES) {
            if (line[0] == 'F' && line.find('{') != std::string::npos) {
                size_t brace = line.find('{');
                current_frame_range = line.substr(1, brace - 1);
            }
            else if (line.find("rgba(") == 0) {
                size_t brace = line.find('{');
                current_color = parse_rgba(line.substr(0, brace));
            }
            else if (line.find("P=") == 0 || line.find("PL=") == 0) {
                std::vector<int> frame_nums = parse_frame_range(current_frame_range);
                for (int fn : frame_nums) {
                    if (fn > 0 && fn <= video_info.total_frames) {
                        frames[fn - 1].frame_number = fn;
                        frames[fn - 1].commands[current_color].push_back(line);
                    }
                }
            }
        }
        else if (state == AUDIO_INFO) {
            if (line.find("hz=") == 0) {
                audio_data.sample_rate = std::stoi(line.substr(3));
                player_state.has_audio = true;
                std::cout << "🎵 Audio sample rate: " << audio_data.sample_rate << "Hz\n";
            }
            else if (line.find("c=") == 0) {
                audio_data.channels = std::stoi(line.substr(2));
                audio_data.channel_data.resize(audio_data.channels);
                std::cout << "🎧 Audio channels: " << audio_data.channels << "\n";
            }
            else if (line.find("sam=") == 0) {
                audio_data.total_samples = std::stoll(line.substr(4));
                std::cout << "📊 Audio samples: " << audio_data.total_samples << "\n";
            }
        }
        else if (state == AUDIO_CHANNELS) {
            if (line[0] == 'C' && line.find('{') != std::string::npos) {
                current_audio_channel = std::stoi(line.substr(1, line.find('{') - 1)) - 1;
            }
            else if (current_audio_channel >= 0 && current_audio_channel < audio_data.channels) {
                parse_audio_channel(line, audio_data.channel_data[current_audio_channel]);
            }
        }
    }
    
    // 🔥 CALCULATE SAMPLES PER FRAME FOR SYNC!!
    if (player_state.has_audio && video_info.total_frames > 0) {
        player_state.samples_per_frame = (double)audio_data.total_samples / video_info.total_frames;
        std::cout << "🎯 Samples per frame: " << player_state.samples_per_frame << "\n";
    }
    
    std::cout << "✅ Parsing complete!!\n";
    return true;
}

// 🎨 DRAW PIXEL COMMAND
void draw_pixel(SDL_Surface* surface, int x, int y, const RGBA& color) {
    if (x < 0 || x >= surface->w || y < 0 || y >= surface->h) return;
    
    Uint32 pixel_color = SDL_MapRGBA(surface->format, color.r, color.g, color.b, color.a);
    Uint32* pixels = (Uint32*)surface->pixels;
    pixels[y * surface->w + x] = pixel_color;
}

// 🎨 DRAW LINE COMMAND
void draw_line(SDL_Surface* surface, int x1, int y1, int x2, int y2, const RGBA& color) {
    if (y1 == y2) {
        for (int x = x1; x <= x2; x++) {
            draw_pixel(surface, x, y1, color);
        }
    } else {
        int dx = abs(x2 - x1);
        int dy = abs(y2 - y1);
        int sx = x1 < x2 ? 1 : -1;
        int sy = y1 < y2 ? 1 : -1;
        int err = dx - dy;
        
        while (true) {
            draw_pixel(surface, x1, y1, color);
            if (x1 == x2 && y1 == y2) break;
            
            int e2 = 2 * err;
            if (e2 > -dy) {
                err -= dy;
                x1 += sx;
            }
            if (e2 < dx) {
                err += dx;
                y1 += sy;
            }
        }
    }
}

// 🎨 RENDER FRAME
void render_frame(SDL_Surface* surface, int frame_idx) {
    if (frame_idx < 0 || frame_idx >= frames.size()) return;
    
    const Frame& frame = frames[frame_idx];
    
    for (const auto& [color, commands] : frame.commands) {
        for (const std::string& cmd : commands) {
            if (cmd.find("PL=") == 0) {
                size_t eq = cmd.find('=');
                size_t dash = cmd.find('-', eq);
                
                std::string start_str = cmd.substr(eq + 1, dash - eq - 1);
                std::string end_str = cmd.substr(dash + 1);
                
                size_t x_pos1 = start_str.find('x');
                int x1 = std::stoi(start_str.substr(0, x_pos1)) - 1;
                int y1 = std::stoi(start_str.substr(x_pos1 + 1)) - 1;
                
                size_t x_pos2 = end_str.find('x');
                int x2 = std::stoi(end_str.substr(0, x_pos2)) - 1;
                int y2 = std::stoi(end_str.substr(x_pos2 + 1)) - 1;
                
                draw_line(surface, x1, y1, x2, y2, color);
            }
            else if (cmd.find("P=") == 0) {
                size_t eq = cmd.find('=');
                std::string pos_str = cmd.substr(eq + 1);
                size_t x_pos = pos_str.find('x');
                
                int x = std::stoi(pos_str.substr(0, x_pos)) - 1;
                int y = std::stoi(pos_str.substr(x_pos + 1)) - 1;
                
                draw_pixel(surface, x, y, color);
            }
        }
    }
}

// 🎵 AUDIO CALLBACK - FIXED FOR SYNC!!
void audio_callback(void* userdata, Uint8* stream, int len) {
    memset(stream, 0, len);
    
    if (!player_state.playing || !player_state.has_audio) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(audio_mutex);
    
    float* output = (float*)stream;
    int samples_needed = len / sizeof(float) / audio_data.channels;
    
    // 🎯 SYNC TO TARGET SAMPLE POSITION FROM VIDEO FRAME!!
    int64_t target = player_state.target_audio_sample.load();
    int64_t drift = target - player_state.audio_sample_pos;
    
    // 🔥 RESYNC if drift is too large (more than 0.1 seconds)
    if (abs(drift) > audio_data.sample_rate / 10) {
        player_state.audio_sample_pos = target;
        std::cout << "🎯 Audio resynced! Drift was: " << drift << " samples\n";
    }
    
    for (int i = 0; i < samples_needed; i++) {
        for (int ch = 0; ch < audio_data.channels; ch++) {
            if (player_state.audio_sample_pos >= 0 && 
                player_state.audio_sample_pos < audio_data.total_samples) {
                output[i * audio_data.channels + ch] = 
                    audio_data.channel_data[ch][player_state.audio_sample_pos];
            } else {
                output[i * audio_data.channels + ch] = 0.0f;
            }
        }
        
        player_state.audio_sample_pos++;
        
        // Loop audio if needed
        if (video_info.loop && player_state.audio_sample_pos >= audio_data.total_samples) {
            player_state.audio_sample_pos = 0;
        }
    }
}

int main(int argc, char* argv[]) {
    std::cout << "🔥🔥🔥 HMICAV MEDIA PLAYER V2.0 - SYNC FIXED!! 🔥🔥🔥\n";
    std::cout << "🎬 Frame-Perfect A/V Synchronization!! 🎵\n";
    std::cout << "💎 Full RGBA Support + Hardware Acceleration!! 💎\n\n";
    
    std::string file_path;
    if (argc > 1) {
        file_path = argv[1];
    } else {
        std::cout << "Enter HMICAV file path (.hmicav or .hmicav7): ";
        std::getline(std::cin, file_path);
    }
    
    // 📂 LOAD FILE
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "❌ Failed to open file\n";
        return 1;
    }
    
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::vector<char> buffer(size);
    if (!file.read(buffer.data(), size)) {
        std::cerr << "❌ Failed to read file\n";
        return 1;
    }
    file.close();
    
    std::cout << "📂 File loaded: " << (size / 1024.0) << " KB\n";
    
    // 🔓 DECOMPRESS IF NEEDED
    std::string content;
    if (file_path.find(".hmicav7") != std::string::npos) {
        std::cout << "🌀 Decompressing Zstd...\n";
        content = decompress_zstd(buffer);
        if (content.empty()) {
            std::cerr << "❌ Decompression failed\n";
            return 1;
        }
        std::cout << "✅ Decompressed to " << (content.size() / 1024.0) << " KB\n";
    } else {
        content = std::string(buffer.begin(), buffer.end());
    }
    
    // 📖 PARSE CONTENT
    if (!parse_hmicav(content)) {
        std::cerr << "❌ Failed to parse HMICAV\n";
        return 1;
    }
    
    // 🎮 INITIALIZE SDL
    std::cout << "\n🎮 Initializing SDL2...\n";
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        std::cerr << "❌ SDL Init failed: " << SDL_GetError() << "\n";
        return 1;
    }
    
    SDL_Window* window = SDL_CreateWindow(
        "HMICAV Player 🔥 - SYNCED!!",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        video_info.width, video_info.height,
        SDL_WINDOW_SHOWN
    );
    
    if (!window) {
        std::cerr << "❌ Window creation failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return 1;
    }
    
    screen_surface = SDL_GetWindowSurface(window);
    
    // 🎵 SETUP AUDIO
    if (player_state.has_audio) {
        std::cout << "🎵 Setting up audio...\n";
        
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = audio_data.sample_rate;
        want.format = AUDIO_F32SYS;
        want.channels = audio_data.channels;
        want.samples = 512;  // Smaller buffer for better sync!!
        want.callback = audio_callback;
        
        player_state.audio_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (player_state.audio_device == 0) {
            std::cerr << "⚠️ Audio setup failed: " << SDL_GetError() << "\n";
            player_state.has_audio = false;
        } else {
            std::cout << "✅ Audio device opened!!\n";
            std::cout << "🎯 Buffer size: " << have.samples << " samples\n";
            SDL_PauseAudioDevice(player_state.audio_device, 0);
        }
    }
    
    std::cout << "\n🎬 Ready to play!!\n";
    std::cout << "⌨️  Controls:\n";
    std::cout << "   SPACE - Play/Pause\n";
    std::cout << "   LEFT/RIGHT - Seek ±10 frames\n";
    std::cout << "   R - Restart\n";
    std::cout << "   ESC - Quit\n\n";
    
    player_state.playing = true;
    player_state.start_time = std::chrono::high_resolution_clock::now();
    
    // 🎬 MAIN LOOP
    SDL_Event event;
    auto last_render = std::chrono::high_resolution_clock::now();
    
    while (!player_state.quit) {
        // Handle events
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                player_state.quit = true;
            }
            else if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                    case SDLK_SPACE: {
                        player_state.playing = !player_state.playing;
                        if (player_state.playing) {
                            // Reset start time when resuming
                            auto now = std::chrono::high_resolution_clock::now();
                            double elapsed_frames = player_state.current_frame * player_state.frame_duration_ms;
                            player_state.start_time = now - std::chrono::milliseconds((int)elapsed_frames);
                        }
                        std::cout << (player_state.playing ? "▶️  PLAY" : "⏸️  PAUSE") << "\n";
                        break;
                    }
                    
                    case SDLK_LEFT: {
                        player_state.current_frame = std::max(0, player_state.current_frame - 10);
                        // 🎯 UPDATE AUDIO TARGET!!
                        int64_t target_sample = (int64_t)(player_state.current_frame * player_state.samples_per_frame);
                        player_state.target_audio_sample.store(target_sample);
                        std::cout << "⏪ Seek to frame " << player_state.current_frame << "\n";
                        
                        // Reset timing
                        auto now = std::chrono::high_resolution_clock::now();
                        double elapsed_frames = player_state.current_frame * player_state.frame_duration_ms;
                        player_state.start_time = now - std::chrono::milliseconds((int)elapsed_frames);
                        break;
                    }
                    
                    case SDLK_RIGHT: {
                        player_state.current_frame = std::min(video_info.total_frames - 1, 
                                                              player_state.current_frame + 10);
                        // 🎯 UPDATE AUDIO TARGET!!
                        int64_t target_sample = (int64_t)(player_state.current_frame * player_state.samples_per_frame);
                        player_state.target_audio_sample.store(target_sample);
                        std::cout << "⏩ Seek to frame " << player_state.current_frame << "\n";
                        
                        // Reset timing
                        auto now = std::chrono::high_resolution_clock::now();
                        double elapsed_frames = player_state.current_frame * player_state.frame_duration_ms;
                        player_state.start_time = now - std::chrono::milliseconds((int)elapsed_frames);
                        break;
                    }
                    
                    case SDLK_r: {
                        player_state.current_frame = 0;
                        player_state.target_audio_sample.store(0);
                        player_state.audio_sample_pos = 0;
                        player_state.start_time = std::chrono::high_resolution_clock::now();
                        std::cout << "🔄 Restart\n";
                        break;
                    }
                    
                    case SDLK_ESCAPE:
                        player_state.quit = true;
                        break;
                }
            }
        }
        
        // 🎯 UPDATE FRAME BASED ON TIME (MASTER CLOCK)
        if (player_state.playing) {
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - player_state.start_time
            ).count();
            
            int target_frame = (int)(elapsed_ms / player_state.frame_duration_ms);
            
            if (target_frame != player_state.current_frame) {
                player_state.current_frame = target_frame;
                
                // 🎯 UPDATE AUDIO TARGET SAMPLE POSITION!!
                int64_t target_sample = (int64_t)(player_state.current_frame * player_state.samples_per_frame);
                player_state.target_audio_sample.store(target_sample);
                
                if (player_state.current_frame >= video_info.total_frames) {
                    if (video_info.loop) {
                        player_state.current_frame = 0;
                        player_state.start_time = now;
                        player_state.target_audio_sample.store(0);
                    } else {
                        player_state.playing = false;
                        player_state.current_frame = video_info.total_frames - 1;
                    }
                }
            }
        }
        
        // 🎨 RENDER at controlled rate (avoid GPU spam)
        auto now = std::chrono::high_resolution_clock::now();
        auto since_render = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_render
        ).count();
        
        if (since_render >= 16) {  // ~60 FPS render cap
            // Clear screen
            SDL_FillRect(screen_surface, nullptr, SDL_MapRGB(screen_surface->format, 0, 0, 0));
            
            // Render current frame
            render_frame(screen_surface, player_state.current_frame);
            
            // Update window
            SDL_UpdateWindowSurface(window);
            last_render = now;
        }
        
        // Small delay to prevent CPU spinning
        SDL_Delay(1);
    }
    
    // Cleanup
    std::cout << "\n🛑 Shutting down...\n";
    
    if (player_state.audio_device != 0) {
        SDL_CloseAudioDevice(player_state.audio_device);
    }
    
    SDL_DestroyWindow(window);
    SDL_Quit();
    
    std::cout << "✨ Thanks for using HMICAV Player!! ✨\n";
    
    return 0;
}