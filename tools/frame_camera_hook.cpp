#include "platform/linux/resource.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <json-c/json.h>
#include <linux/dma-buf.h>
#include <linux/udmabuf.h>
#include <linux/videodev2.h>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unordered_map>

namespace {
using ct::FileDescriptor;
using Ioctl = int (*)(int, unsigned long, ...);
Ioctl ioctl_next() {
    static auto next = reinterpret_cast<Ioctl>(dlsym(RTLD_NEXT, "ioctl"));
    return next;
}

constexpr auto Capture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
constexpr unsigned MinBuffers = 8;
constexpr double Tolerance = .001;
constexpr double TimerHz = 19200000.;
constexpr size_t FrontViews = 2;
constexpr size_t ViewCount = 4;
constexpr unsigned FrontMask = (1U << FrontViews) - 1;
constexpr unsigned AllMask = (1U << ViewCount) - 1;

// The front pair defines an exposure. An upper view must fit the same 1 ms window.
constexpr unsigned synchronized_views(std::array<double, ViewCount> sof, unsigned required) {
    double first = std::min(sof[0], sof[1]), last = std::max(sof[0], sof[1]);
    if (!first || last - first > Tolerance)
        return 0;
    unsigned mask = FrontMask;
    for (size_t i = FrontViews; i < ViewCount; ++i) {
        const double low = std::min(first, sof[i]), high = std::max(last, sof[i]);
        if (sof[i] && high - low <= Tolerance) {
            mask |= 1U << i;
            first = low;
            last = high;
        }
    }
    return (mask & required) == required ? mask : 0;
}
static_assert(synchronized_views({1, 1, 1, 1}, AllMask) == AllMask);
static_assert(!synchronized_views({1, 1, 0, 1}, AllMask));
static_assert(synchronized_views({1, 1, 0, 1}, 1U << 3) == 0b1011);
static_assert(!synchronized_views({1, 1, 0, 1}, 1U << 2));
static_assert(synchronized_views({1, 1, 1.004, .996}, FrontMask) == FrontMask);
static_assert(!synchronized_views({1, 1, .9992, 1.0008}, AllMask));
static_assert(!synchronized_views({1, 1.004, 1, 1}, FrontMask));
static_assert(!synchronized_views({0, 1, 1, 1}, FrontMask));

// In 50/60 Hz lighting modes the scene follows IR by 21.62/24.95 ms respectively.
constexpr bool scene_gap(double seconds) {
    return seconds >= .020 && seconds <= .026;
}
static_assert(scene_gap(.021622) && scene_gap(.024955));
static_assert(!scene_gap(.008378) && !scene_gap(.030) && !scene_gap(1. / 30));

struct LumaPlane {
    uint32_t stride = 0, size = 0;

    constexpr bool fits(unsigned width, unsigned height, size_t capacity) const {
        return stride >= width && stride <= 8192 && size <= capacity && stride &&
               height <= size / stride;
    }
};

// Frame's CAMSS driver programs this layout independently of VIDIOC_G_FMT.bytesperline.
struct CamssPlaneLayout {
    uint32_t offset[4], stride[4], size[4];

    constexpr LumaPlane linear_luma() const {
        return offset[0] == 0 && !size[2] && !size[3] ? LumaPlane{stride[0], size[0]} : LumaPlane{};
    }
};
constexpr auto SetPlaneLayout = _IOW('V', BASE_VIDIOC_PRIVATE + 6, CamssPlaneLayout);
static_assert(CamssPlaneLayout{{0, 1179648}, {1152, 1152}, {1179648, 589824}}.linear_luma().fits(
    1056, 1024, 1769472));
static_assert(!LumaPlane{1152, 1056 * 1024}.fits(1056, 1024, 1769472));

struct CameraRoute {
    std::string_view grey, alternate;
    uint32_t alternate_format;

    constexpr uint32_t format(std::string_view node) const {
        if (node == grey)
            return V4L2_PIX_FMT_GREY;
        return node == alternate ? alternate_format : 0;
    }
};
constexpr CameraRoute SlamRight{"msm_vfe3_video0", "msm_vfe0_video0", V4L2_PIX_FMT_NV12};
constexpr CameraRoute SlamLeft{"msm_vfe4_video0", "msm_vfe1_video0", V4L2_PIX_FMT_NV12};
// Without FPGA interleaving, the 640x480 upper cameras can use the fronts' grayscale nodes.
constexpr CameraRoute UpperLeft{"msm_vfe2_video0", SlamLeft.grey, V4L2_PIX_FMT_GREY};
constexpr CameraRoute UpperRight{"msm_vfe2_video1", SlamRight.grey, V4L2_PIX_FMT_GREY};
static_assert(SlamRight.format("msm_vfe0_video0") == V4L2_PIX_FMT_NV12 &&
              SlamLeft.format("msm_vfe1_video0") == V4L2_PIX_FMT_NV12 &&
              SlamRight.format("msm_vfe3_video0") == V4L2_PIX_FMT_GREY &&
              SlamLeft.format("msm_vfe4_video0") == V4L2_PIX_FMT_GREY &&
              !SlamLeft.format("msm_vfe0_video0"));
static_assert(UpperLeft.format("msm_vfe2_video0") == V4L2_PIX_FMT_GREY &&
              UpperRight.format("msm_vfe2_video1") == V4L2_PIX_FMT_GREY &&
              UpperLeft.format("msm_vfe4_video0") == V4L2_PIX_FMT_GREY &&
              UpperRight.format("msm_vfe3_video0") == V4L2_PIX_FMT_GREY &&
              !UpperLeft.format("msm_vfe3_video0") && !UpperRight.format("msm_vfe4_video0"));

struct Camera {
    const char *name;
    CameraRoute route;
    unsigned width, height;
    size_t dma_size;
    std::string serial;
    int device = -1;
    double last_sof = 0;
    std::optional<LumaPlane> isp_luma = std::nullopt;
    uint32_t stride = 0;
};

struct Backing {
    FileDescriptor file;
    ino_t inode;
    uint64_t offset;
};

struct View {
    FileDescriptor file, dma;
    ino_t inode = 0;
    uint64_t offset = 0;
    uint32_t stride = 0;
    double sof = 0, time = 0;
    int device = -1;
    v4l2_buffer queue{};
    std::array<v4l2_plane, 2> planes{};
};

double clock_seconds(clockid_t clock) {
    timespec now{};
    clock_gettime(clock, &now);
    return now.tv_sec + now.tv_nsec * 1e-9;
}

bool sync_read(int dma, uint64_t phase) {
    dma_buf_sync request{DMA_BUF_SYNC_READ | phase};
    int result;
    do {
        result = ioctl_next()(dma, DMA_BUF_IOCTL_SYNC, &request);
    } while (result < 0 && (errno == EINTR || errno == EAGAIN));
    return result == 0;
}

double sof_time(const v4l2_plane &metadata) {
    constexpr size_t Size = sizeof(uint64_t);
    if (metadata.data_offset || metadata.bytesused < Size ||
        !sync_read(metadata.m.fd, DMA_BUF_SYNC_START))
        return 0;
    void *bytes = mmap(nullptr, Size, PROT_READ, MAP_SHARED, metadata.m.fd, 0);
    uint64_t timestamp = 0;
    if (bytes != MAP_FAILED) {
        memcpy(&timestamp, bytes, Size);
        munmap(bytes, Size);
    }
    const bool synced = sync_read(metadata.m.fd, DMA_BUF_SYNC_END);
    // Qualcomm CAMSS frame header stores the ISP SOF counter in bits 63:8.
    return synced && !(timestamp & 0xff) ? (timestamp >> 8) / TimerHz : 0;
}

double capture_time(const v4l2_buffer &buffer, double sof) {
    // The Frame's CAMSS VFE 880 dequeues with ktime_get_raw_ns().
    const double end = buffer.timestamp.tv_sec + buffer.timestamp.tv_usec * 1e-6;
    const double raw = clock_seconds(CLOCK_MONOTONIC_RAW);
    if (end < raw - .100 || end > raw + .002)
        return 0;
#if defined(__aarch64__)
    uint64_t counter;
    asm volatile("mrs %0, cntvct_el0" : "=r"(counter));
    const double start = raw + (sof - counter / TimerHz);
    if (start <= end + .002 && end - start <= .025)
        return start;
#else
    (void)sof;
#endif
    return end;
}

struct Group {
    std::array<View, ViewCount> views;
    uint64_t seq = 0;
};

class CameraHook {
    using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
    std::mutex mutex;
    std::array<Camera, ViewCount> cameras{{
        {"slam_right", SlamRight, 1056, 1024, 1769472, {}},
        {"slam_left", SlamLeft, 1056, 1024, 1769472, {}},
        {"upper_left", UpperLeft, 640, 480, 462848, {}},
        {"upper_right", UpperRight, 640, 480, 462848, {}},
    }};
    std::unordered_map<int, Backing> backing;
    FileDescriptor lock, listener, reader, pidfd;
    std::string calibration, path;
    std::array<Group, 2> pending;
    Group leased;
    uint64_t sequence = 0;
    std::array<double, 2> last_published{};

    void clear(View &view) {
        if (view.dma)
            sync_read(view.dma.get(), DMA_BUF_SYNC_END);
        if (view.queue.length) {
            view.queue.m.planes = view.planes.data();
            if (ioctl_next()(view.device, VIDIOC_QBUF, &view.queue) < 0)
                dprintf(2, "Controller Utils camera: delayed QBUF failed: %s\n", strerror(errno));
        }
        view = {};
    }

    void clear(Group &group) {
        for (auto &view : group.views)
            clear(view);
        group.seq = 0;
    }

    void service_reader() {
        if (reader) {
            char data[64];
            const ssize_t size = recv(reader.get(), data, sizeof(data), MSG_DONTWAIT | MSG_TRUNC);
            char expected[64];
            const int count = snprintf(expected, sizeof(expected), "{\"release\": %llu}",
                                       static_cast<unsigned long long>(leased.seq));
            if (leased.seq && size == count && !memcmp(data, expected, count))
                clear(leased);
            else if (!size || size > 0 || (errno != EAGAIN && errno != EINTR))
                reader.reset();
        }
        if (leased.seq) {
            pollfd state{pidfd.get(), POLLIN, 0};
            if (poll(&state, 1, 0) > 0 && state.revents & POLLIN) {
                reader.reset();
                clear(leased);
            }
        }
        if (!reader) {
            for (auto &group : pending)
                clear(group);
            if (leased.seq)
                return;
            pidfd.reset();
            reader.reset(accept4(listener.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (!reader)
                return;
            ucred peer{};
            socklen_t size = sizeof(peer);
            int process = -1;
            if (getsockopt(reader.get(), SOL_SOCKET, SO_PEERCRED, &peer, &size) ||
                peer.uid != geteuid()) {
                reader.reset();
                return;
            }
            size = sizeof(process);
            if (getsockopt(reader.get(), SOL_SOCKET, SO_PEERPIDFD, &process, &size) < 0 ||
                send(reader.get(), calibration.data(), calibration.size(), MSG_NOSIGNAL) !=
                    static_cast<ssize_t>(calibration.size())) {
                FileDescriptor failed(process);
                reader.reset();
                return;
            }
            pidfd.reset(process);
            last_published = {};
        }
    }

    void publish(bool ir, unsigned required) {
        if (leased.seq)
            return;
        auto &group = pending[ir];
        std::array<double, ViewCount> sof{};
        for (size_t i = 0; i < ViewCount; ++i)
            sof[i] = group.views[i].sof;
        const auto selected = synchronized_views(sof, required);
        if (!selected)
            return;
        std::array<int, ViewCount> descriptors{};
        size_t count = 0;
        double time = 0;
        std::string views;
        for (size_t i = 0; i < ViewCount; ++i) {
            if (!(selected & (1U << i)))
                continue;
            const auto &view = group.views[i];
            if (i < FrontViews)
                time += view.time / FrontViews;
            descriptors[count] = view.file.get();
            views += (count ? ",[\"" : "[\"") + cameras[i].serial + "\"," +
                     std::to_string(view.stride) + "," + std::to_string(view.offset) + "]";
            ++count;
        }
        const uint64_t seq = sequence + 1;
        std::string header = "{\"seq\":" + std::to_string(seq) +
                             ",\"time\":" + std::to_string(time) + ",\"exposure\":\"" +
                             (ir ? "ir" : "scene") + "\",\"storage\":\"udmabuf\",\"views\":[" +
                             views + "]}";
        alignas(cmsghdr) char control[CMSG_SPACE(ViewCount * sizeof(int))]{};
        iovec body{header.data(), header.size()};
        msghdr packet{};
        packet.msg_iov = &body;
        packet.msg_iovlen = 1;
        packet.msg_control = control;
        packet.msg_controllen = CMSG_SPACE(count * sizeof(int));
        auto *rights = CMSG_FIRSTHDR(&packet);
        rights->cmsg_level = SOL_SOCKET;
        rights->cmsg_type = SCM_RIGHTS;
        rights->cmsg_len = CMSG_LEN(count * sizeof(int));
        memcpy(CMSG_DATA(rights), descriptors.data(), count * sizeof(int));
        if (sendmsg(reader.get(), &packet, MSG_NOSIGNAL) == static_cast<ssize_t>(header.size())) {
            sequence = leased.seq = seq;
            last_published[ir] = std::max(sof[0], sof[1]);
            for (size_t i = 0; i < ViewCount; ++i)
                if (selected & (1U << i))
                    leased.views[i] = std::exchange(group.views[i], View{});
        } else {
            clear(group);
        }
    }

    void dequeue(size_t index, const v4l2_buffer &buffer) {
        auto &camera = cameras[index];
        if (buffer.memory != V4L2_MEMORY_DMABUF || buffer.length != 2 ||
            (buffer.flags & (V4L2_BUF_FLAG_TIMESTAMP_MASK | V4L2_BUF_FLAG_ERROR)) !=
                V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
            return;
        service_reader();
        if (!reader)
            return;
        const double sof = sof_time(buffer.m.planes[1]);
        if (!sof)
            return;
        const double gap = sof - std::exchange(camera.last_sof, sof);
        const bool ir = gap > .007 && gap < .010;
        const bool front = index < FrontViews;
        if (!ir && (!front || !scene_gap(gap)))
            return;
        if (sof <= last_published[ir] + Tolerance)
            return;
        auto &view = pending[ir].views[index];
        if (sof <= view.sof + Tolerance)
            return;
        const double time = capture_time(buffer, sof);
        if (!time)
            return;
        const auto &pixels = buffer.m.planes[0];
        const auto source = backing.find(pixels.m.fd);
        struct stat dma{};
        if (source == backing.end() || pixels.data_offset ||
            pixels.bytesused < camera.stride * camera.height || fstat(pixels.m.fd, &dma) < 0 ||
            dma.st_ino != source->second.inode)
            return;
        FileDescriptor shared(fcntl(source->second.file.get(), F_DUPFD_CLOEXEC, 3));
        FileDescriptor lease(fcntl(pixels.m.fd, F_DUPFD_CLOEXEC, 3));
        if (!shared || !lease || !sync_read(lease.get(), DMA_BUF_SYNC_START))
            return;
        if (view.file) {
            // Share a synchronized IR group before any of its views is replaced.
            if (ir)
                publish(true, 1U << index);
            clear(view);
        }
        view.sof = sof;
        view.file = std::move(shared);
        view.dma = std::move(lease);
        view.inode = dma.st_ino;
        view.offset = source->second.offset;
        view.stride = camera.stride;
        view.time = time - (ir ? 0 : .00836);
        view.device = camera.device;
        publish(ir, ir ? AllMask : FrontMask);
    }

  public:
    explicit CameraHook(const char *config) {
        std::ifstream input(config);
        std::string packet;
        if (!std::getline(input, packet) || packet.empty() || packet.size() > 131072)
            return;
        Json parsed(json_tokener_parse(packet.c_str()), json_object_put);
        json_object *version, *metadata, *entries;
        if (!parsed || !json_object_object_get_ex(parsed.get(), "version", &version) ||
            json_object_get_int(version) != 1 ||
            !json_object_object_get_ex(parsed.get(), "metadata", &metadata) ||
            !json_object_object_get_ex(metadata, "cameras", &entries))
            return;
        for (auto &camera : cameras) {
            json_object *entry, *serial;
            if (!json_object_object_get_ex(entries, camera.name, &entry) ||
                !json_object_object_get_ex(entry, "serialNumber", &serial) ||
                !json_object_is_type(serial, json_type_string))
                return;
            camera.serial = json_object_get_string(serial);
            if (camera.serial.empty() || camera.serial.size() > 64 ||
                camera.serial.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") !=
                    std::string::npos)
                return;
        }
        const auto uid = geteuid();
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        std::string directory = runtime ? runtime : "/run/user/" + std::to_string(uid);
        struct stat info{};
        if (stat(directory.c_str(), &info) < 0 || !S_ISDIR(info.st_mode) || info.st_uid != uid ||
            (info.st_mode & 077))
            return;
        path = directory + "/controller-utils-camera.sock";
        sockaddr_un address{};
        if (path.size() >= sizeof(address.sun_path))
            return;
        lock.reset(open((path + ".lock").c_str(), O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_RDWR, 0600));
        if (!lock || flock(lock.get(), LOCK_EX | LOCK_NB) < 0)
            return;
        if (lstat(path.c_str(), &info) == 0) {
            if (!S_ISSOCK(info.st_mode) || info.st_uid != uid)
                return;
            FileDescriptor probe(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
            address.sun_family = AF_UNIX;
            memcpy(address.sun_path, path.c_str(), path.size() + 1);
            if (!probe ||
                connect(probe.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) ==
                    0 ||
                errno != ECONNREFUSED)
                return;
            if (unlink(path.c_str()) < 0)
                return;
        }
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, path.c_str(), path.size() + 1);
        listener.reset(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (!listener ||
            bind(listener.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
            chmod(path.c_str(), 0600) < 0 || listen(listener.get(), 2) < 0) {
            listener.reset();
            return;
        }
        calibration = std::move(packet);
        dprintf(2, "Controller Utils camera: sharing enabled\n");
    }

    void created(int fd, const udmabuf_create &request) {
        if (!listener || std::none_of(cameras.begin(), cameras.end(), [&](const auto &camera) {
                return request.size == camera.dma_size;
            }))
            return;
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/fd/%u", request.memfd);
        FileDescriptor source(open(path, O_RDONLY | O_CLOEXEC));
        struct stat file{}, dma{};
        if (!source || fstat(source.get(), &file) < 0 || fstat(fd, &dma) < 0 ||
            !S_ISREG(file.st_mode) || file.st_size < 0 ||
            request.offset > static_cast<uint64_t>(file.st_size) ||
            request.size > static_cast<uint64_t>(file.st_size) - request.offset)
            return;
        const int seals = fcntl(source.get(), F_GET_SEALS);
        if (seals < 0 || !(seals & F_SEAL_SHRINK))
            return;
        std::lock_guard guard(mutex);
        for (auto entry = backing.begin(); entry != backing.end();) {
            struct stat current{};
            if (fstat(entry->first, &current) < 0 || current.st_ino != entry->second.inode)
                entry = backing.erase(entry);
            else
                ++entry;
        }
        if (backing.size() < 256)
            backing.insert_or_assign(fd, Backing{std::move(source), dma.st_ino, request.offset});
    }

    int camera_ioctl(int fd, unsigned long request, void *arg) {
        auto next = ioctl_next();
        if (!listener || !arg)
            return next(fd, request, arg);
        if (request == VIDIOC_S_FMT || request == SetPlaneLayout || request == VIDIOC_REQBUFS) {
            std::lock_guard guard(mutex);
            int result = next(fd, request, arg);
            const int error = errno;
            struct stat dev{};
            if (!result && !fstat(fd, &dev) && S_ISCHR(dev.st_mode) && major(dev.st_rdev) == 81) {
                std::ifstream device_name("/sys/dev/char/81:" + std::to_string(minor(dev.st_rdev)) +
                                          "/name");
                std::string video_name;
                std::getline(device_name, video_name);
                for (auto &camera : cameras) {
                    const uint32_t pixel_format = camera.route.format(video_name);
                    if (request != VIDIOC_REQBUFS) {
                        if (pixel_format == V4L2_PIX_FMT_NV12) {
                            if (request == SetPlaneLayout)
                                camera.isp_luma =
                                    static_cast<CamssPlaneLayout *>(arg)->linear_luma();
                            else if (static_cast<v4l2_format *>(arg)->type == Capture)
                                camera.isp_luma.reset();
                        }
                        continue;
                    }
                    const auto &buffers = *static_cast<v4l2_requestbuffers *>(arg);
                    if (buffers.type != Capture)
                        continue;
                    // XRService reuses descriptor numbers when switching camera routes.
                    if (camera.device == fd)
                        camera.device = -1;
                    if (!pixel_format || buffers.count < MinBuffers ||
                        buffers.memory != V4L2_MEMORY_DMABUF)
                        continue;
                    v4l2_format format{};
                    format.type = Capture;
                    const auto &pixels = format.fmt.pix_mp;
                    if (next(fd, VIDIOC_G_FMT, &format) || pixels.width != camera.width ||
                        pixels.height != camera.height || pixels.pixelformat != pixel_format ||
                        pixels.num_planes != 2)
                        continue;
                    const auto &first = pixels.plane_fmt[0];
                    const LumaPlane reported{first.bytesperline, first.sizeimage};
                    const auto plane = pixel_format == V4L2_PIX_FMT_NV12
                                           ? camera.isp_luma.value_or(reported)
                                           : reported;
                    if (!plane.fits(camera.width, camera.height,
                                    std::min<size_t>(first.sizeimage, camera.dma_size)))
                        continue;
                    camera.device = fd;
                    camera.stride = plane.stride;
                    camera.last_sof = 0;
                }
            }
            errno = error;
            return result;
        }
        if (request == VIDIOC_DQBUF) {
            const int result = next(fd, request, arg);
            const int error = errno;
            const auto &buffer = *static_cast<v4l2_buffer *>(arg);
            if (!result && buffer.type == Capture && buffer.m.planes) {
                std::lock_guard guard(mutex);
                for (size_t i = 0; i < cameras.size(); ++i)
                    if (cameras[i].device == fd) {
                        dequeue(i, buffer);
                        break;
                    }
            }
            errno = error;
            return result;
        }

        std::lock_guard guard(mutex);
        const auto camera = std::find_if(cameras.begin(), cameras.end(),
                                         [fd](const auto &item) { return item.device == fd; });
        if (camera == cameras.end())
            return next(fd, request, arg);
        const auto index = static_cast<size_t>(camera - cameras.begin());
        if (request == VIDIOC_STREAMOFF) {
            const int result = next(fd, request, arg);
            const int error = errno;
            if (result == 0) {
                leased.views[index].queue = {};
                for (auto &group : pending) {
                    group.views[index].queue = {};
                    clear(group);
                }
                camera->last_sof = 0;
            }
            errno = error;
            return result;
        }
        auto &buffer = *static_cast<v4l2_buffer *>(arg);
        if (buffer.type == Capture && buffer.memory == V4L2_MEMORY_DMABUF && buffer.length == 2 &&
            buffer.m.planes) {
            service_reader();
            for (auto *group : {&leased, &pending[0], &pending[1]}) {
                auto &view = group->views[index];
                struct stat dma{};
                if (view.file && !fstat(buffer.m.planes[0].m.fd, &dma) &&
                    dma.st_ino == view.inode) {
                    // The remaining buffers continue streaming even if the reader is suspended.
                    view.queue = buffer;
                    std::copy_n(buffer.m.planes, 2, view.planes.begin());
                    return 0;
                }
            }
        }
        return next(fd, request, arg);
    }
};

CameraHook *camera_hook() {
    static auto *hook = []() -> CameraHook * {
        const char *config = getenv("CONTROLLER_UTILS_CAMERA_CALIBRATION");
        char executable[1024]{};
        if (!config || readlink("/proc/self/exe", executable, sizeof(executable) - 1) <= 0)
            return nullptr;
        const char *name = strrchr(executable, '/');
        return name && !strcmp(name, "/XRService") ? new CameraHook(config) : nullptr;
    }();
    return hook;
}
} // namespace

extern "C" __attribute__((visibility("default"))) int ioctl(int fd, unsigned long request, ...) {
    va_list arguments;
    va_start(arguments, request);
    const auto arg = va_arg(arguments, uintptr_t);
    va_end(arguments);
    const int previous = errno;
    if (request == VIDIOC_S_FMT || request == SetPlaneLayout || request == VIDIOC_REQBUFS ||
        request == VIDIOC_DQBUF || request == VIDIOC_QBUF || request == VIDIOC_STREAMOFF) {
        if (auto *hook = camera_hook()) {
            errno = previous;
            return hook->camera_ioctl(fd, request, reinterpret_cast<void *>(arg));
        }
    }
    errno = previous;
    auto next = ioctl_next();
    const int result = next(fd, request, arg);
    const int error = errno;
    if (result >= 0 && request == UDMABUF_CREATE && arg)
        if (auto *hook = camera_hook())
            hook->created(result, *reinterpret_cast<const udmabuf_create *>(arg));
    errno = error;
    return result;
}
