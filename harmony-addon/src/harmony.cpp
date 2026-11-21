// HarmonyOS video and single-contact control through an authenticated local bridge.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <gamescrcpy_ext.h>
#include "coordinate.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
const gs_ext_host* host = nullptr;
std::atomic<int> width{0}, height{0};
std::atomic<unsigned long long> frameCount{}, packetCount{};
std::mutex videosMutex;
void diagnostic(const std::string& text)
{
    host->log(host->context, GS_EXT_LOG_INFO, text.data(), text.size());
}
void notifyDimensions(int w, int h);

std::string environment(const char* name)
{
    char value[512]{};
    DWORD count = GetEnvironmentVariableA(name, value, sizeof(value));
    return count > 0 && count < sizeof(value) ? std::string(value, count) : std::string();
}

bool sendAll(SOCKET socket, const char* data, size_t size)
{
    while (size) {
        int count = send(socket, data, static_cast<int>(size), 0);
        if (count <= 0) return false;
        data += count; size -= static_cast<size_t>(count);
    }
    return true;
}

bool receiveAll(SOCKET socket, uint8_t* data, size_t size, const std::atomic<bool>* stopping = nullptr,
                ULONGLONG idleLimit = 5000)
{
    ULONGLONG progress = GetTickCount64();
    while (size) {
        if (stopping && stopping->load()) return false;
        int count = recv(socket, reinterpret_cast<char*>(data), static_cast<int>(size), 0);
        if (count == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if ((error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) && GetTickCount64() - progress < idleLimit)
                continue;
            return false;
        }
        if (count == 0) return false;
        progress = GetTickCount64();
        data += count; size -= static_cast<size_t>(count);
    }
    return true;
}

SOCKET connectBridge(const char* channel)
{
    const std::string port = environment("GS_HARMONY_PORT"), token = environment("GS_HARMONY_TOKEN");
    const int number = std::atoi(port.c_str());
    if (number < 1024 || number > 65535 || token.size() < 32) return INVALID_SOCKET;
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) return socket;
    DWORD timeout = 5000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    int yes = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(static_cast<u_short>(number));
    if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(socket); return INVALID_SOCKET;
    }
    const std::string hello = std::string("GSH1 ") + channel + " " + token + "\n";
    if (!sendAll(socket, hello.data(), hello.size())) { closesocket(socket); return INVALID_SOCKET; }
    return socket;
}

struct Video {
    uint64_t stream{};
    std::atomic<bool> stopping{};
    std::atomic<SOCKET> socket{INVALID_SOCKET};
    std::thread worker;
    std::mutex retryMutex;
    std::condition_variable retry;
    void stop() {
        stopping = true;
        retry.notify_all();
        SOCKET old = socket.load();
        if (old != INVALID_SOCKET) shutdown(old, SD_BOTH);
        if (worker.joinable()) worker.join();
    }
};
std::map<uint64_t, std::shared_ptr<Video>> videos;
std::map<uint64_t, std::shared_ptr<Video>> audios;
std::mutex audiosMutex;
std::atomic<unsigned long long> audioFrames{};

bool receiveLine(SOCKET socket, std::string& text, const std::atomic<bool>* stopping)
{
    for (int i = 0; i < 64; ++i) {
        uint8_t c;
        if (!receiveAll(socket, &c, 1, stopping, 10000)) return false;
        if (c == '\n') return true;
        text.push_back(static_cast<char>(c));
    }
    return false;
}

bool audioSession(const std::shared_ptr<Video>& audio)
{
    bool played = false;
    SOCKET socket = connectBridge("AUDIO");
    audio->socket = socket;
    if (socket != INVALID_SOCKET && !audio->stopping) {
        DWORD timeout = 200;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::string format;
        bool good = receiveLine(socket, format, &audio->stopping) && format == "GSA1 48000 2 S16LE";
        std::vector<uint8_t> pcm;
        while (good && !audio->stopping) {
            uint8_t header[4];
            if (!receiveAll(socket, header, 4, &audio->stopping, 10000)) break;
            uint32_t size = (uint32_t(header[0]) << 24) | (uint32_t(header[1]) << 16) |
                (uint32_t(header[2]) << 8) | header[3];
            if (size < 4 || size > 192000 || size % 4) break;
            pcm.resize(size);
            if (!receiveAll(socket, pcm.data(), pcm.size(), &audio->stopping, 10000)) break;
            gs_ext_audio samples{};
            samples.size = sizeof(samples); samples.format = GS_EXT_SAMPLE_S16;
            samples.channels = 2; samples.rate = GS_EXT_SOUND_RATE;
            samples.data = pcm.data(); samples.frames = size / 4;
            samples.time_us = host->now_us(host->context);
            if (audio->stopping || host->audio_samples(host->context, audio->stream, &samples) != GS_EXT_OK) break;
            if (audioFrames.fetch_add(samples.frames) == 0) diagnostic("HarmonyOS playback audio started: 48000 Hz, stereo S16LE");
            played = true;
        }
    }
    SOCKET old = audio->socket.exchange(INVALID_SOCKET);
    if (old != INVALID_SOCKET) { shutdown(old, SD_BOTH); closesocket(old); }
    return played;
}
void audioRun(const std::shared_ptr<Video>& audio)
{
    ULONGLONG deadline = GetTickCount64() + 60000;
    while (!audio->stopping) {
        bool played = audioSession(audio);
        if (played || audio->stopping || GetTickCount64() >= deadline) break;
        std::unique_lock lock(audio->retryMutex);
        audio->retry.wait_for(lock, std::chrono::seconds(1), [&] { return audio->stopping.load(); });
    }
    if (!audio->stopping) {
        const char error[] = "HarmonyOS audio unavailable. Start the signed phone companion and allow playback capture.";
        host->stream_ended(host->context, audio->stream, error, sizeof(error) - 1);
    }
}

int32_t listAudio(void*, gs_ext_result* result)
{
    const char* text = environment("GS_HARMONY_AUDIO_PORT").empty() ? "[]" :
        "[{\"id\":\"usb\",\"name\":\"HarmonyOS playback audio\"}]";
    return host->set_text(result, GS_EXT_TYPE_JSON, text, std::strlen(text));
}
int32_t openAudio(void*, const char* id, size_t size, const char*, size_t, uint64_t stream)
{
    if (std::string(id, size) != "usb") return GS_EXT_BAD_ARGUMENT;
    if (environment("GS_HARMONY_AUDIO_PORT").empty() || !host->audio_samples) return GS_EXT_UNSUPPORTED;
    std::lock_guard lock(audiosMutex); if (!audios.empty()) return GS_EXT_BUSY;
    auto audio = std::make_shared<Video>(); audio->stream = stream;
    audios.emplace(stream, audio); audio->worker = std::thread(audioRun, audio);
    return GS_EXT_OK;
}
void closeAudio(void*, uint64_t stream)
{
    std::shared_ptr<Video> audio;
    { std::lock_guard lock(audiosMutex); auto it = audios.find(stream); if (it == audios.end()) return;
      audio = it->second; audios.erase(it); }
    audio->stop();
}

bool publish(Video& video, AVFrame* picture, std::vector<uint8_t>& converted)
{
    const int w = picture->width, h = picture->height;
    if (w < 2 || h < 2 || w > 4096 || h > 4096 || w % 2 || h % 2 || w * h > 10000000) return false;
    gs_ext_video_frame frame{};
    frame.size = sizeof(frame); frame.format = GS_EXT_PIXEL_NV12;
    frame.width = static_cast<uint32_t>(w); frame.height = static_cast<uint32_t>(h);
    frame.time_us = host->now_us(host->context);
    if (picture->format == AV_PIX_FMT_NV12 && picture->linesize[0] > 0 && picture->linesize[1] > 0) {
        frame.data = picture->data[0]; frame.stride = static_cast<uint32_t>(picture->linesize[0]);
        frame.chroma = picture->data[1]; frame.chroma_stride = static_cast<uint32_t>(picture->linesize[1]);
    } else if (picture->format == AV_PIX_FMT_YUV420P || picture->format == AV_PIX_FMT_YUVJ420P) {
        converted.resize(static_cast<size_t>(w) * h * 3 / 2);
        for (int row = 0; row < h; row++)
            std::memcpy(converted.data() + static_cast<size_t>(row) * w, picture->data[0] + row * picture->linesize[0], w);
        uint8_t* uv = converted.data() + static_cast<size_t>(w) * h;
        for (int row = 0; row < h / 2; row++) for (int x = 0; x < w / 2; x++) {
            uv[row * w + x * 2] = picture->data[1][row * picture->linesize[1] + x];
            uv[row * w + x * 2 + 1] = picture->data[2][row * picture->linesize[2] + x];
        }
        frame.data = converted.data(); frame.stride = static_cast<uint32_t>(w);
        frame.chroma = uv; frame.chroma_stride = static_cast<uint32_t>(w);
    } else return false;
    bool resized = width.exchange(w) != w;
    resized = (height.exchange(h) != h) || resized;
    if (resized) notifyDimensions(w, h);
    int32_t result = host->video_frame(host->context, video.stream, &frame);
    if (result == GS_EXT_OK) ++frameCount;
    return result == GS_EXT_OK;
}

void videoSession(const std::shared_ptr<Video>& video)
{
    SOCKET socket = connectBridge("VIDEO");
    if (socket == INVALID_SOCKET) {
        return;
    }
    video->socket = socket;
    if (video->stopping) { closesocket(video->socket.exchange(INVALID_SOCKET)); return; }
    DWORD timeout = 200;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    AVCodecContext* codec = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
    AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_H264);
    AVFrame* picture = av_frame_alloc(); AVPacket* packet = av_packet_alloc();
    std::vector<uint8_t> data, converted;
    bool good = codec && parser && picture && packet;
    if (good) {
        codec->thread_count = 2; codec->thread_type = FF_THREAD_SLICE; codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
        good = avcodec_open2(codec, nullptr, nullptr) >= 0;
    }
    while (good && !video->stopping) {
        uint8_t header[4]; if (!receiveAll(socket, header, sizeof(header), &video->stopping, 45000)) break;
        uint32_t size = (uint32_t(header[0]) << 24) | (uint32_t(header[1]) << 16) | (uint32_t(header[2]) << 8) | header[3];
        if (!size) continue;
        if (size > 8 * 1024 * 1024) break;
        data.assign(static_cast<size_t>(size) + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        if (!receiveAll(socket, data.data(), size, &video->stopping, 45000)) break;
        ++packetCount;
        const uint8_t* next = data.data(); int remaining = static_cast<int>(size);
        for (int iteration = 0; remaining > 0 && iteration < 128; iteration++) {
            int used = av_parser_parse2(parser, codec, &packet->data, &packet->size, next, remaining,
                                       AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (used < 0) { good = false; break; }
            next += used; remaining -= used;
            if (packet->size > 0 && avcodec_send_packet(codec, packet) >= 0) {
                while (avcodec_receive_frame(codec, picture) == 0) {
                    if (!video->stopping && !publish(*video, picture, converted)) good = false;
                    av_frame_unref(picture);
                }
            }
            if (!used && !packet->size) break;
        }
    }
    av_packet_free(&packet); av_frame_free(&picture);
    if (parser) av_parser_close(parser);
    avcodec_free_context(&codec);
    SOCKET old = video->socket.exchange(INVALID_SOCKET);
    if (old != INVALID_SOCKET) { shutdown(old, SD_BOTH); closesocket(old); }
}
void videoRun(const std::shared_ptr<Video>& video)
{
    for (int attempt = 0; attempt < 4 && !video->stopping; ++attempt) {
        videoSession(video);
        if (video->stopping) return;
        if (attempt < 3) {
            diagnostic("HarmonyOS video disconnected; retry " + std::to_string(attempt + 1));
            std::unique_lock lock(video->retryMutex);
            video->retry.wait_for(lock, std::chrono::milliseconds(500 << attempt),
                [&] { return video->stopping.load(); });
        }
    }
    if (!video->stopping) {
        const char error[] = "HarmonyOS capture disconnected. Check USB authorization and unlock the phone, then reopen the mirror.";
        host->stream_ended(host->context, video->stream, error, sizeof(error) - 1);
    }
}

int32_t listVideo(void*, gs_ext_result* result)
{
    const char* text = environment("GS_HARMONY_TOKEN").empty() ? "[]" : "[{\"id\":\"usb\",\"name\":\"HarmonyOS USB (single touch)\"}]";
    return host->set_text(result, GS_EXT_TYPE_JSON, text, std::strlen(text));
}
int32_t openVideo(void*, const char* id, size_t size, const char*, size_t, uint64_t stream)
{
    if (std::string(id, size) != "usb") return GS_EXT_BAD_ARGUMENT;
    std::lock_guard lock(videosMutex); if (!videos.empty()) return GS_EXT_BUSY;
    auto video = std::make_shared<Video>(); video->stream = stream;
    videos.emplace(stream, video); video->worker = std::thread(videoRun, video);
    return GS_EXT_OK;
}
void closeVideo(void*, uint64_t stream)
{
    std::shared_ptr<Video> video;
    { std::lock_guard lock(videosMutex); auto it = videos.find(stream); if (it == videos.end()) return;
      video = it->second; videos.erase(it); }
    video->stop();
}

struct Touch {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::string> pending;
    std::atomic<bool> stopping{};
    std::atomic<bool> failed{};
    std::atomic<SOCKET> socket{INVALID_SOCKET};
    std::thread worker;
    bool held{};
    uint32_t contactId{};
    int32_t enqueue(std::string text) {
        std::lock_guard lock(mutex);
        if (stopping || failed || pending.size() >= 256) { failed = true; changed.notify_one(); return GS_EXT_FAILED; }
        if (text.starts_with("T 1 ") && !pending.empty() && pending.back().starts_with("T 1 ")) {
            pending.back() = std::move(text); changed.notify_one(); return GS_EXT_OK;
        }
        pending.push_back(std::move(text)); changed.notify_one(); return GS_EXT_OK;
    }
};
std::mutex touchMutex;
Touch* activeTouch = nullptr;
void notifyDimensions(int w, int h)
{
    std::lock_guard lock(touchMutex);
    if (activeTouch) activeTouch->enqueue("D " + std::to_string(w) + " " + std::to_string(h) + "\n");
}
void touchRun(Touch* touch)
{
    SOCKET socket = connectBridge("CONTROL");
    if (socket == INVALID_SOCKET) { diagnostic("HarmonyOS control connection failed"); touch->failed = true; return; }
    diagnostic("HarmonyOS control channel opened");
    DWORD timeout = 200;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    touch->socket = socket;
    while (true) {
        std::unique_lock lock(touch->mutex);
        touch->changed.wait_for(lock, std::chrono::seconds(1), [touch] { return touch->stopping || touch->failed || !touch->pending.empty(); });
        if (touch->stopping || touch->failed) break;
        std::string command = "P\n";
        if (!touch->pending.empty()) { command = std::move(touch->pending.front()); touch->pending.pop_front(); }
        lock.unlock();
        uint8_t acknowledgment[3];
        if (!sendAll(socket, command.data(), command.size()) ||
            !receiveAll(socket, acknowledgment, 3, &touch->stopping) || std::memcmp(acknowledgment, "OK\n", 3)) {
            diagnostic("HarmonyOS control acknowledgment failed"); touch->failed = true; break;
        }
    }
    sendAll(socket, "R\n", 2);
    SOCKET old = touch->socket.exchange(INVALID_SOCKET);
    if (old != INVALID_SOCKET) { shutdown(old, SD_BOTH); closesocket(old); }
}
int32_t openTouch(void*, const gs_ext_touch_open*, void** state)
{
    if (environment("GS_HARMONY_TOKEN").empty()) return GS_EXT_FAILED;
    std::lock_guard lock(touchMutex); if (activeTouch) return GS_EXT_BUSY;
    auto* touch = new Touch;
    touch->pending.push_back("D " + std::to_string(width.load()) + " " + std::to_string(height.load()) + "\n");
    activeTouch = touch; *state = touch;
    touch->worker = std::thread(touchRun, touch); return GS_EXT_OK;
}
void closeTouch(void*, void* state)
{
    auto* touch = static_cast<Touch*>(state); if (!touch) return;
    { std::lock_guard lock(touch->mutex); touch->stopping = true; touch->changed.notify_one(); }
    SOCKET socket = touch->socket.load(); if (socket != INVALID_SOCKET) shutdown(socket, SD_RECEIVE);
    if (touch->worker.joinable()) touch->worker.join();
    { std::lock_guard lock(touchMutex); if (activeTouch == touch) activeTouch = nullptr; }
    delete touch;
}
int32_t touchFrame(void*, void* state, const gs_ext_touch_frame* frame)
{
    if (!state || !frame || frame->count > 64 || (frame->count && !frame->contacts)) return GS_EXT_BAD_ARGUMENT;
    auto* touch = static_cast<Touch*>(state);
    std::lock_guard lock(touch->mutex);
    if (touch->stopping || touch->failed) return GS_EXT_FAILED;
    bool held = touch->held;
    uint32_t contactId = touch->contactId;
    std::vector<std::string> commands;
    for (uint32_t i = 0; i < frame->count; i++) {
        const auto& point = frame->contacts[i];
        int x = pixelCoordinate(point.x, width.load()), y = pixelCoordinate(point.y, height.load());
        if (x < 0 || y < 0 || point.action > GS_EXT_TOUCH_CANCEL) return GS_EXT_BAD_ARGUMENT;
        if (point.action == GS_EXT_TOUCH_DOWN) {
            if (held) return GS_EXT_UNSUPPORTED;
            held = true; contactId = point.id;
        } else {
            if (!held || point.id != contactId) return GS_EXT_BAD_ARGUMENT;
            if (point.action == GS_EXT_TOUCH_UP || point.action == GS_EXT_TOUCH_CANCEL) held = false;
        }
        char command[96]; int n = std::snprintf(command, sizeof(command), "T %u %d %d\n", point.action, x, y);
        commands.emplace_back(command, static_cast<size_t>(n));
    }
    if (touch->pending.size() + commands.size() > 256) {
        touch->failed = true; touch->changed.notify_one(); return GS_EXT_FAILED;
    }
    for (auto& command : commands) {
        if (command.starts_with("T 1 ") && !touch->pending.empty() && touch->pending.back().starts_with("T 1 "))
            touch->pending.back() = std::move(command);
        else touch->pending.push_back(std::move(command));
    }
    touch->held = held; touch->contactId = contactId;
    touch->changed.notify_one(); return GS_EXT_OK;
}
int32_t touchKey(void*, void* state, const gs_ext_key* key)
{
    if (!key || !state) return GS_EXT_BAD_ARGUMENT;
    if (!key->down || key->repeat || (key->keycode != 3 && key->keycode != 4)) return GS_EXT_OK;
    return static_cast<Touch*>(state)->enqueue("K " + std::to_string(key->keycode) + "\n");
}
int32_t capabilities(void*, const gs_ext_value*, uint32_t, gs_ext_result* result)
{
    const std::string text = std::string("{\"platform\":\"HarmonyOS\",\"maxContacts\":1,\"gestureContacts\":1,\"audio\":") +
        (environment("GS_HARMONY_AUDIO_PORT").empty() ? "false" : "true") +
        ",\"audioRequiresCompanion\":true,\"realtimeMultiTouch\":false,\"experimental\":true}";
    return host->set_text(result, GS_EXT_TYPE_JSON, text.data(), text.size());
}
int32_t statistics(void*, const gs_ext_value*, uint32_t, gs_ext_result* result)
{
    const std::string text = "{\"frames\":" + std::to_string(frameCount.load()) + ",\"packets\":" +
        std::to_string(packetCount.load()) + ",\"width\":" + std::to_string(width.load()) +
        ",\"height\":" + std::to_string(height.load()) + ",\"audioFrames\":" + std::to_string(audioFrames.load()) + "}";
    return host->set_text(result, GS_EXT_TYPE_JSON, text.data(), text.size());
}
void unload(void*)
{
    while (true) {
        uint64_t stream;
        { std::lock_guard lock(videosMutex); if (videos.empty()) break; stream = videos.begin()->first; }
        closeVideo(nullptr, stream);
    }
    if (activeTouch) closeTouch(nullptr, activeTouch);
    while (true) {
        uint64_t stream;
        { std::lock_guard lock(audiosMutex); if (audios.empty()) break; stream = audios.begin()->first; }
        closeAudio(nullptr, stream);
    }
    WSACleanup();
}
const gs_ext_video_source videoSource{sizeof(gs_ext_video_source), "hdc", nullptr, listVideo, openVideo, closeVideo};
const gs_ext_audio_source audioSource{sizeof(gs_ext_audio_source), "hdc", nullptr, listAudio, openAudio, closeAudio};
const gs_ext_touch_method touchMethod{sizeof(gs_ext_touch_method), "hdc", 1, 1, 0, nullptr,
    openTouch, closeTouch, touchFrame, touchKey, nullptr, nullptr, nullptr};
const gs_ext_node capabilityNode{sizeof(gs_ext_node), "capabilities", nullptr, 0, GS_EXT_TYPE_JSON, nullptr, capabilities};
const gs_ext_node statsNode{sizeof(gs_ext_node), "stats", nullptr, 0, GS_EXT_TYPE_JSON, nullptr, statistics};
const gs_ext_contribution contributions[]{
    {sizeof(gs_ext_contribution), GS_EXT_KIND_VIDEO_SOURCE, "hdc", &videoSource},
    {sizeof(gs_ext_contribution), GS_EXT_KIND_AUDIO_SOURCE, "hdc", &audioSource},
    {sizeof(gs_ext_contribution), GS_EXT_KIND_TOUCH_METHOD, "hdc", &touchMethod},
    {sizeof(gs_ext_contribution), GS_EXT_KIND_NODE, "capabilities", &capabilityNode},
    {sizeof(gs_ext_contribution), GS_EXT_KIND_NODE, "stats", &statsNode}
};
const gs_ext_plugin plugin{sizeof(gs_ext_plugin), GS_EXT_ABI_VERSION, nullptr, 5, contributions, unload};
}
GS_EXT_EXPORT int32_t gs_ext_main(const gs_ext_host* api, const gs_ext_plugin** out)
{
    if (!api || !out || api->abi_version != GS_EXT_ABI_VERSION || !api->supports ||
        !api->supports(api->context, "media")) return GS_EXT_UNSUPPORTED;
    WSADATA data; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return GS_EXT_FAILED;
    host = api; *out = &plugin; return GS_EXT_OK;
}
