#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <cstring>
#include <thread>
#include <atomic>

// 🎮 SDL2 FOR RENDERING + AUDIO
#include <SDL2/SDL.h>

// 🚀 ZSTD DECOMPRESSION (for compressed frames)
#include <zstd.h>

// 🗺️ MEMORY MAPPING FOR ULTRA SPEED!!
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// 🎨 RGBA COLOR STRUCT
struct RGBA {
    uint8_t r, g, b, a;
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

// 🎮 ULTRA-FAST PLAYER STATE!!
struct FastPlayerState {
    bool playing = false;
    bool has_audio = false;
    int current_frame = 0;
    std::atomic<bool> quit{false};
    SDL_AudioDeviceID audio_device = 0;
    
    // 🔥 MEMORY-MAPPED FILE DATA!!
    void* mapped_data = nullptr;
    size_t mapped_size = 0;
    int fd = -1;
    
    // 📍 POINTERS INTO MAPPED MEMORY (ZERO-COPY!!)
    HMICFastHeader* header = nullptr;
    FrameIndexEntry* frame_index = nullptr;
    uint8_t* frames_base = nullptr;
    float* audio_data = nullptr;
    
    // 🎨 FRAME CACHE (for decompressed frames if needed)
    std::vector<RGBA*> frame_cache;
    std::vector<bool> frame_cached;
    
    // 🎵 AUDIO TIMING - SMOOTH AND SYNCED!!
    std::atomic<int64_t> audio_frame_position{0}; // Which frame audio is playing
    double samples_per_frame = 0;
    int64_t audio_sample_index = 0;
};

FastPlayerState player;

// 🎵 AUDIO CALLBACK - IMPROVED SMOOTH PLAYBACK!!
void audio_callback(void* userdata, Uint8* stream, int len) {
    memset(stream, 0, len);
    
    if (!player.playing || !player.has_audio || !player.audio_data) {
        return;
    }
    
    float* output = (float*)stream;
    int samples_needed = len / sizeof(float) / player.header->audio_channels;
    
    // 💨 SMOOTH CONTINUOUS PLAYBACK - NO JUMPING!!
    for (int i = 0; i < samples_needed; i++) {
        if (player.audio_sample_index >= 0 && 
            player.audio_sample_index < (int64_t)player.header->audio_samples) {
            
            // 🔥 DIRECT MEMORY ACCESS - INSTANT!!
            for (int ch = 0; ch < player.header->audio_channels; ch++) {
                int64_t sample_idx = player.audio_sample_index * player.header->audio_channels + ch;
                output[i * player.header->audio_channels + ch] = player.audio_data[sample_idx];
            }
        } else {
            for (int ch = 0; ch < player.header->audio_channels; ch++) {
                output[i * player.header->audio_channels + ch] = 0.0f;
            }
        }
        
        player.audio_sample_index++;
        
        // Update which frame audio is at
        if (player.samples_per_frame > 0) {
            player.audio_frame_position.store((int64_t)(player.audio_sample_index / player.samples_per_frame));
        }
        
        // Loop if needed
        if (player.audio_sample_index >= (int64_t)player.header->audio_samples) {
            player.audio_sample_index = 0;
            player.audio_frame_position.store(0);
        }
    }
}

// ⚡ LOAD HMICFAST FILE WITH MEMORY MAPPING - INSTANT LOAD!!
bool load_hmicfast(const std::string& path) {
    std::cout << "⚡⚡⚡ LOADING WITH MEMORY MAPPING!! ⚡⚡⚡\n";
    std::cout << "🔥 ZERO-COPY INSTANT ACCESS!! 🔥\n\n";
    
    // Open file
    player.fd = open(path.c_str(), O_RDONLY);
    if (player.fd == -1) {
        std::cerr << "❌ Failed to open file\n";
        return false;
    }
    
    // Get file size
    struct stat sb;
    if (fstat(player.fd, &sb) == -1) {
        std::cerr << "❌ Failed to get file size\n";
        close(player.fd);
        return false;
    }
    player.mapped_size = sb.st_size;
    
    std::cout << "📂 File size: " << (player.mapped_size / 1024.0 / 1024.0) << " MB\n";
    
    // 🗺️ MEMORY MAP THE ENTIRE FILE - THIS IS THE SAUCE!!
    player.mapped_data = mmap(nullptr, player.mapped_size, PROT_READ, 
                              MAP_PRIVATE, player.fd, 0);
    
    if (player.mapped_data == MAP_FAILED) {
        std::cerr << "❌ Memory mapping failed\n";
        close(player.fd);
        return false;
    }
    
    std::cout << "✅ FILE MEMORY-MAPPED!! INSTANT ACCESS UNLOCKED!! 💚\n\n";
    
    // 📍 SET UP POINTERS INTO MAPPED MEMORY (NO COPYING!!)
    player.header = (HMICFastHeader*)player.mapped_data;
    
    // Verify magic header
    if (memcmp(player.header->magic, "HMICFAST", 8) != 0) {
        std::cerr << "❌ Invalid HMICFAST file (bad magic header)\n";
        munmap(player.mapped_data, player.mapped_size);
        close(player.fd);
        return false;
    }
    
    std::cout << "🎬 VIDEO INFO:\n";
    std::cout << "   📺 Resolution: " << player.header->width << "x" << player.header->height << "\n";
    std::cout << "   🎞️  FPS: " << player.header->fps << "\n";
    std::cout << "   📊 Total frames: " << player.header->total_frames << "\n";
    std::cout << "   💾 Compression: " << (player.header->compressed ? "Zstd" : "None (RAW)") << "\n";
    
    // 📍 POINT TO FRAME INDEX TABLE (NO LOADING NEEDED!!)
    player.frame_index = (FrameIndexEntry*)((uint8_t*)player.mapped_data + 
                                            player.header->frame_index_offset);
    
    // 📍 FRAMES BASE POINTER
    player.frames_base = (uint8_t*)player.mapped_data + sizeof(HMICFastHeader) + 
                         (sizeof(FrameIndexEntry) * player.header->total_frames);
    
    std::cout << "✅ Frame index mapped!! " << player.header->total_frames << " frames ready\n";
    
    // 🎵 SETUP AUDIO IF PRESENT
    if (player.header->has_audio) {
        std::cout << "\n🎵 AUDIO INFO:\n";
        std::cout << "   🎧 Sample rate: " << player.header->audio_sample_rate << "Hz\n";
        std::cout << "   📊 Channels: " << (int)player.header->audio_channels << "\n";
        std::cout << "   🎼 Total samples: " << player.header->audio_samples << "\n";
        
        // 📍 POINT DIRECTLY TO AUDIO DATA (ZERO-COPY!!)
        player.audio_data = (float*)((uint8_t*)player.mapped_data + 
                                     player.header->audio_data_offset);
        
        player.has_audio = true;
        player.samples_per_frame = (double)player.header->audio_samples / player.header->total_frames;
        
        std::cout << "   🎯 Samples per frame: " << player.samples_per_frame << "\n";
        std::cout << "✅ Audio data mapped!! INSTANT ACCESS!! 💚\n";
    } else {
        std::cout << "\n🔇 No audio in this file\n";
    }
    
    // 🎨 SETUP FRAME CACHE FOR DECOMPRESSION
    if (player.header->compressed) {
        std::cout << "\n📦 Frame compression detected - allocating cache...\n";
        player.frame_cache.resize(player.header->total_frames, nullptr);
        player.frame_cached.resize(player.header->total_frames, false);
        std::cout << "✅ Cache ready for on-demand decompression\n";
    }
    
    std::cout << "\n🔥🔥🔥 LOADING COMPLETE!! READY TO GO BRRRRR!! 🔥🔥🔥\n";
    
    return true;
}

// ⚡ GET FRAME DATA - ULTRA FAST!!
RGBA* get_frame_data(int frame_idx) {
    if (frame_idx < 0 || frame_idx >= (int)player.header->total_frames) {
        return nullptr;
    }
    
    // 🔥 IF NOT COMPRESSED - DIRECT POINTER!! INSTANT!! ⚡⚡⚡
    if (!player.header->compressed) {
        return (RGBA*)(player.frames_base + player.frame_index[frame_idx].offset - 
                      (sizeof(HMICFastHeader) + sizeof(FrameIndexEntry) * player.header->total_frames));
    }
    
    // 📦 IF COMPRESSED - CHECK CACHE FIRST
    if (player.frame_cached[frame_idx]) {
        return player.frame_cache[frame_idx];
    }
    
    // 🌀 DECOMPRESS AND CACHE
    uint8_t* compressed_data = player.frames_base + player.frame_index[frame_idx].offset - 
                               (sizeof(HMICFastHeader) + sizeof(FrameIndexEntry) * player.header->total_frames);
    size_t compressed_size = player.frame_index[frame_idx].size;
    
    size_t frame_size = player.header->width * player.header->height * sizeof(RGBA);
    player.frame_cache[frame_idx] = (RGBA*)malloc(frame_size);
    
    size_t result = ZSTD_decompress(player.frame_cache[frame_idx], frame_size,
                                    compressed_data, compressed_size);
    
    if (ZSTD_isError(result)) {
        std::cerr << "❌ Decompression error for frame " << frame_idx << "\n";
        free(player.frame_cache[frame_idx]);
        player.frame_cache[frame_idx] = nullptr;
        return nullptr;
    }
    
    player.frame_cached[frame_idx] = true;
    return player.frame_cache[frame_idx];
}

// 🧹 CLEANUP
void cleanup() {
    std::cout << "\n🧹 Cleaning up...\n";
    
    // Free frame cache if used
    if (player.header && player.header->compressed) {
        for (size_t i = 0; i < player.frame_cache.size(); i++) {
            if (player.frame_cache[i]) {
                free(player.frame_cache[i]);
            }
        }
    }
    
    // Unmap memory
    if (player.mapped_data != nullptr && player.mapped_data != MAP_FAILED) {
        munmap(player.mapped_data, player.mapped_size);
        std::cout << "✅ Memory unmapped\n";
    }
    
    // Close file
    if (player.fd != -1) {
        close(player.fd);
        std::cout << "✅ File closed\n";
    }
}

int main(int argc, char* argv[]) {
    std::cout << "⚡⚡⚡ HMIC-FAST GPU ACCELERATED PLAYER ⚡⚡⚡\n";
    std::cout << "🔥 HARDWARE RENDERING + ZERO-COPY PLAYBACK!! 🔥\n";
    std::cout << "💨 BUTTERY SMOOTH 60FPS!! 💨\n\n";
    
    std::string file_path;
    if (argc > 1) {
        file_path = argv[1];
    } else {
        std::cout << "Enter HMICFAST file path (.hmicfast): ";
        std::getline(std::cin, file_path);
    }
    
    // ⚡ LOAD WITH MEMORY MAPPING - INSTANT!!
    if (!load_hmicfast(file_path)) {
        return 1;
    }
    
    // 🎮 INITIALIZE SDL WITH HARDWARE ACCELERATION!!
    std::cout << "\n🎮 Initializing SDL2 with GPU acceleration...\n";
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        std::cerr << "❌ SDL Init failed: " << SDL_GetError() << "\n";
        cleanup();
        return 1;
    }
    
    // 🚀 CREATE WINDOW
    SDL_Window* window = SDL_CreateWindow(
        "HMIC-FAST Player ⚡ - GPU TURBO MODE!!",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        player.header->width, player.header->height,
        SDL_WINDOW_SHOWN
    );
    
    if (!window) {
        std::cerr << "❌ Window creation failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        cleanup();
        return 1;
    }
    
    // 🔥🔥🔥 CREATE HARDWARE ACCELERATED RENDERER!! 🔥🔥🔥
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, 
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    
    if (!renderer) {
        std::cerr << "⚠️  Hardware acceleration failed, trying software...\n";
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer) {
            std::cerr << "❌ Renderer creation failed: " << SDL_GetError() << "\n";
            SDL_DestroyWindow(window);
            SDL_Quit();
            cleanup();
            return 1;
        }
    } else {
        std::cout << "✅ GPU HARDWARE ACCELERATION ENABLED!! 🚀🚀🚀\n";
    }
    
    // Get renderer info
    SDL_RendererInfo info;
    SDL_GetRendererInfo(renderer, &info);
    std::cout << "🎨 Renderer: " << info.name << "\n";
    std::cout << "   Hardware accelerated: " << (info.flags & SDL_RENDERER_ACCELERATED ? "YES ✅" : "NO") << "\n";
    std::cout << "   VSync enabled: " << (info.flags & SDL_RENDERER_PRESENTVSYNC ? "YES ✅" : "NO") << "\n";
    
    // 🎨 CREATE STREAMING TEXTURE FOR ULTRA-FAST GPU UPLOADS!!
    SDL_Texture* texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING,
        player.header->width,
        player.header->height);
    
    if (!texture) {
        std::cerr << "❌ Texture creation failed: " << SDL_GetError() << "\n";
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        cleanup();
        return 1;
    }
    
    std::cout << "✅ Streaming texture created!! GPU READY!! 💚\n";
    
    // 🎵 SETUP AUDIO
    if (player.has_audio) {
        std::cout << "\n🎵 Setting up audio...\n";
        
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = player.header->audio_sample_rate;
        want.format = AUDIO_F32SYS;
        want.channels = player.header->audio_channels;
        want.samples = 1024; // Slightly larger buffer for stability
        want.callback = audio_callback;
        
        player.audio_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (player.audio_device == 0) {
            std::cerr << "⚠️ Audio setup failed: " << SDL_GetError() << "\n";
            player.has_audio = false;
        } else {
            std::cout << "✅ Audio device opened!!\n";
            std::cout << "   Buffer size: " << have.samples << " samples\n";
            SDL_PauseAudioDevice(player.audio_device, 0); // Start audio
        }
    }
    
    std::cout << "\n🎬 READY TO GO ULTRA FAST!!\n";
    std::cout << "⌨️  CONTROLS:\n";
    std::cout << "   SPACE - Play/Pause\n";
    std::cout << "   LEFT/RIGHT - Seek ±1 frame\n";
    std::cout << "   UP/DOWN - Seek ±10 frames\n";
    std::cout << "   HOME - Jump to start\n";
    std::cout << "   END - Jump to end\n";
    std::cout << "   R - Restart\n";
    std::cout << "   ESC - Quit\n\n";
    
    player.playing = true;
    
    // 🔥 PRELOAD FIRST FEW FRAMES IF COMPRESSED
    if (player.header->compressed) {
        std::cout << "🚀 Preloading first 30 frames...\n";
        for (int i = 0; i < std::min(30, (int)player.header->total_frames); i++) {
            get_frame_data(i);
        }
        std::cout << "✅ Preload complete!\n\n";
    }
    
    std::cout << "▶️  PLAYING!! 🔥🔥🔥\n\n";
    
    // 🎬 MAIN LOOP - GPU ACCELERATED SMOOTH AS BUTTER!!
    SDL_Event event;
    int last_rendered_frame = -1;
    
    while (!player.quit) {
        // Handle events
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                player.quit = true;
            }
            else if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                    case SDLK_SPACE: {
                        player.playing = !player.playing;
                        std::cout << (player.playing ? "▶️  PLAY" : "⏸️  PAUSE") << "\n";
                        if (!player.playing && player.has_audio) {
                            SDL_PauseAudioDevice(player.audio_device, 1);
                        } else if (player.playing && player.has_audio) {
                            SDL_PauseAudioDevice(player.audio_device, 0);
                        }
                        break;
                    }
                    
                    case SDLK_LEFT: {
                        player.current_frame = std::max(0, player.current_frame - 1);
                        if (player.has_audio) {
                            player.audio_sample_index = (int64_t)(player.current_frame * player.samples_per_frame);
                            player.audio_frame_position.store(player.current_frame);
                        }
                        break;
                    }
                    
                    case SDLK_RIGHT: {
                        player.current_frame = std::min((int)player.header->total_frames - 1, 
                                                        player.current_frame + 1);
                        if (player.has_audio) {
                            player.audio_sample_index = (int64_t)(player.current_frame * player.samples_per_frame);
                            player.audio_frame_position.store(player.current_frame);
                        }
                        break;
                    }
                    
                    case SDLK_UP: {
                        player.current_frame = std::min((int)player.header->total_frames - 1, 
                                                        player.current_frame + 10);
                        if (player.has_audio) {
                            player.audio_sample_index = (int64_t)(player.current_frame * player.samples_per_frame);
                            player.audio_frame_position.store(player.current_frame);
                        }
                        std::cout << "⏩ Frame " << player.current_frame << "\n";
                        break;
                    }
                    
                    case SDLK_DOWN: {
                        player.current_frame = std::max(0, player.current_frame - 10);
                        if (player.has_audio) {
                            player.audio_sample_index = (int64_t)(player.current_frame * player.samples_per_frame);
                            player.audio_frame_position.store(player.current_frame);
                        }
                        std::cout << "⏪ Frame " << player.current_frame << "\n";
                        break;
                    }
                    
                    case SDLK_HOME: {
                        player.current_frame = 0;
                        if (player.has_audio) {
                            player.audio_sample_index = 0;
                            player.audio_frame_position.store(0);
                        }
                        std::cout << "⏮️  Jump to start\n";
                        break;
                    }
                    
                    case SDLK_END: {
                        player.current_frame = player.header->total_frames - 1;
                        if (player.has_audio) {
                            player.audio_sample_index = (int64_t)(player.current_frame * player.samples_per_frame);
                            player.audio_frame_position.store(player.current_frame);
                        }
                        std::cout << "⏭️  Jump to end\n";
                        break;
                    }
                    
                    case SDLK_r: {
                        player.current_frame = 0;
                        if (player.has_audio) {
                            player.audio_sample_index = 0;
                            player.audio_frame_position.store(0);
                        }
                        std::cout << "🔄 Restart\n";
                        break;
                    }
                    
                    case SDLK_ESCAPE:
                        player.quit = true;
                        break;
                }
            }
        }
        
        // 🎯 SYNC VIDEO TO AUDIO (AUDIO IS MASTER CLOCK!)
        if (player.playing) {
            if (player.has_audio) {
                // Video follows audio - smooth and stable!
                int64_t audio_frame = player.audio_frame_position.load();
                player.current_frame = (int)audio_frame;
                
                if (player.current_frame >= (int)player.header->total_frames) {
                    player.current_frame = 0;
                    player.audio_sample_index = 0;
                    player.audio_frame_position.store(0);
                }
            } else {
                // No audio - advance frame based on FPS
                static auto last_frame_time = std::chrono::high_resolution_clock::now();
                auto now = std::chrono::high_resolution_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_frame_time).count();
                
                double frame_time_ms = 1000.0 / player.header->fps;
                if (elapsed >= frame_time_ms) {
                    player.current_frame++;
                    if (player.current_frame >= (int)player.header->total_frames) {
                        player.current_frame = 0;
                    }
                    last_frame_time = now;
                }
            }
        }
        
        // 🎨 RENDER FRAME WITH GPU!! ONLY IF CHANGED!!
        if (last_rendered_frame != player.current_frame) {
            RGBA* frame_data = get_frame_data(player.current_frame);
            if (frame_data) {
                // 🔥🔥🔥 UPLOAD TO GPU TEXTURE - STREAMING FAST!! 🔥🔥🔥
                void* pixels;
                int pitch;
                SDL_LockTexture(texture, nullptr, &pixels, &pitch);
                
                // Direct memcpy - ultra fast!!
                memcpy(pixels, frame_data, 
                       player.header->width * player.header->height * sizeof(RGBA));
                
                SDL_UnlockTexture(texture);
                
                // 🚀 GPU RENDER!! INSTANT!!
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, nullptr, nullptr);
                SDL_RenderPresent(renderer); // VSync automatically handles timing!
                
                last_rendered_frame = player.current_frame;
            }
        }
        
        // Tiny sleep to prevent CPU spinning (VSync handles main timing)
        SDL_Delay(1);
    }
    
    // Cleanup
    std::cout << "\n🛑 Shutting down...\n";
    
    if (player.audio_device != 0) {
        SDL_CloseAudioDevice(player.audio_device);
    }
    
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    cleanup();
    
    std::cout << "✨ Thanks for using HMIC-FAST GPU Player!! ✨\n";
    std::cout << "🔥 SMOOTH AS BUTTER!! 🔥\n";
    
    return 0;
}