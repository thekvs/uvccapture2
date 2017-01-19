#include <unistd.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>

#include <linux/limits.h>
#include <linux/types.h>          /* for videodev2.h */
#include <linux/videodev2.h>

#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <vector>
#include <utility>
#include <memory>
#include <cerrno>

#include "cxxopts/cxxopts.hpp"
#include "easylogging++/easylogging++.h"

using OptionsPtr = std::shared_ptr<cxxopts::Options>;

INITIALIZE_EASYLOGGINGPP

class V4L2Device {
public:

    V4L2Device() = delete;

    V4L2Device(OptionsPtr opts):
        options(opts)
    {
    }

    ~V4L2Device()
    {
        if (fd != -1) {
            close(fd);
        }
    }

    bool initialize()
    {
        auto initialized = open_device() and check_capabilities() and set_format() and init_buffers();

        return initialized;
    }

    bool capture()
    {
        struct v4l2_buffer bufferinfo;
        bool status = true;

        std::memset(&bufferinfo, 0, sizeof(bufferinfo));

        bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        bufferinfo.memory = V4L2_MEMORY_MMAP;
        bufferinfo.index = 0; /* Queueing buffer index 0. */

        // Put the buffer in the incoming queue.
        if (ioctl(fd, VIDIOC_QBUF, &bufferinfo) < 0) {
            LOG(ERROR) << "VIDIOC_QBUF failed: " << strerror(errno);
            return false;
        }

        // Activate streaming
        int type = bufferinfo.type;
        if (ioctl(fd, VIDIOC_STREAMON, &type) < 0){
            LOG(ERROR) << "VIDIOC_STREAMON failed: " << strerror(errno);
            return false;
        }

        int frames_skipped = 0;

        auto loop = (*options)["loop"].as<bool>();
        int frames_count = options->count("count") ? (*options)["count"].as<int>() : 1;
        int frames_to_skip = options->count("skip") ? (*options)["skip"].as<int>() : 0;
        useconds_t pause = std::lround((options->count("pause") ? (*options)["pause"].as<double>() : 0) * 1e6);

        while ((frames_taken < frames_count) or loop) {
            // Dequeue the buffer.
            if (ioctl(fd, VIDIOC_DQBUF, &bufferinfo) < 0) {
                LOG(ERROR) << "VIDIOC_QBUF failed: " << strerror(errno);
                status = false;
                break;
            }

            bool skip_frame = frames_to_skip > 0 and frames_skipped < frames_to_skip;

            if (not skip_frame) {
                auto ok = write_jpeg(bufferinfo);
                if (not ok) {
                    status = false;
                    break;
                }
            } else {
                frames_skipped++;
            }

            bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            bufferinfo.memory = V4L2_MEMORY_MMAP;
            /* Set the index if using several buffers */

            // Queue the next one.
            if (ioctl(fd, VIDIOC_QBUF, &bufferinfo) < 0) {
                LOG(ERROR) << "VIDIOC_QBUF failed: " << strerror(errno);
                status = false;
                break;
            }

            if (pause > 0) {
                usleep(pause);
            }
        }

        // Deactivate streaming
        if (ioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
            LOG(ERROR) << "VIDIOC_STREAMOFF failed: " << strerror(errno);
            return false;
        }

        return status;
    }

private:
    class IOBuffer
    {
    public:
        IOBuffer(const IOBuffer&) = delete;
        IOBuffer() = default;

        ~IOBuffer()
        {
            if (start != nullptr and size > 0) {
                auto rc = munmap(start, size);
                if (rc < 0) {
                    LOG(ERROR) << "munmap() failed: " << strerror(errno);
                }
            }
        }

        void *start = nullptr;
        size_t size = 0;
    };

    int fd = -1;
    int frames_taken = 0;

    IOBuffer buffer;

    OptionsPtr options;

    bool open_device()
    {
        if (fd == -1) {
            auto device = (*options)["device"].as<std::string>().c_str();
            fd = open(device, O_RDWR);
            if (fd < 0) {
                LOG(ERROR) << "Couldn't open '" << device << "' :" << strerror(errno) << std::endl;
                return false;
            }
        } else {
            LOG(WARNING) << "Is device already initialized?";
            return false;
        }

        return true;
    }

    bool check_capabilities()
    {
        struct v4l2_capability cap;
        std::memset(&cap, 0, sizeof(cap));

        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
            LOG(ERROR) << "VIDIOC_QUERYCAP failed: " << strerror(errno);
            return false;
        }

        if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
            LOG(ERROR) << "the device does not handle single-planar video capture";
            return false;
        }

        if ((cap.capabilities & V4L2_CAP_STREAMING) == 0) {
            LOG(ERROR) << "the device does not handle frame streaming";
            return false;
        }

        return true;
    }

    bool set_format()
    {
        struct v4l2_format format;
        std::memset(&format, 0, sizeof(format));

        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        bool ok;

        std::tie(ok, format.fmt.pix.width, format.fmt.pix.height) = parse_resolution();

        if (not ok) {
            return false;
        }

        if (ioctl(fd, VIDIOC_S_FMT, &format) < 0) {
            LOG(ERROR) << "VIDIOC_S_FMT failed: " << strerror(errno);
            return false;
        }

        return true;
    }

    bool init_buffers()
    {
        struct v4l2_requestbuffers bufrequest;
        std::memset(&bufrequest, 0, sizeof(bufrequest));

        bufrequest.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        bufrequest.memory = V4L2_MEMORY_MMAP;
        bufrequest.count = 1;

        if (ioctl(fd, VIDIOC_REQBUFS, &bufrequest) < 0) {
            LOG(ERROR) << "VIDIOC_REQBUFS failed: " << strerror(errno);
            return false;
        }

        struct v4l2_buffer bufferinfo;
        std::memset(&bufferinfo, 0, sizeof(bufferinfo));

        bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        bufferinfo.memory = V4L2_MEMORY_MMAP;
        bufferinfo.index = 0;

        if (ioctl(fd, VIDIOC_QUERYBUF, &bufferinfo) < 0) {
            LOG(ERROR) << "VIDIOC_QUERYBUF failed: " << strerror(errno);
            return false;
        }

        buffer.start = mmap(
            NULL,
            bufferinfo.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd,
            bufferinfo.m.offset
        );

        if (buffer.start == MAP_FAILED) {
            LOG(ERROR) << "mmaping buffer failed: " << strerror(errno);
            return false;
        }

        buffer.size = bufferinfo.length;
        std::memset(buffer.start, 0, bufferinfo.length);

        return true;
    }

    std::tuple<bool, uint32_t, uint32_t> parse_resolution()
    {
        uint32_t x = 0, y = 0;
        bool ok = true;

        auto resolution = (*options)["resolution"].as<std::string>();
        auto delimeter = resolution.find("x");

        if ((delimeter != std::string::npos) and (delimeter + 1 < resolution.size())) {
            auto x_str = resolution.substr(0, delimeter);
            auto y_str = resolution.substr(delimeter + 1, resolution.size() - 1);

            try {
                x = std::stoul(x_str);
                y = std::stoul(y_str);
            } catch (std::invalid_argument &exc) {
                LOG(ERROR) << "no conversion could be performed: " << exc.what();
                ok = false;
            } catch (std::out_of_range &exc) {
                LOG(ERROR) << "out if range error: " << exc.what();
                ok = false;
            }
        } else {
            LOG(ERROR) << "invalid resolution description: " << resolution;
            ok = false;
        }

        return std::make_tuple(ok, x, y);
    }

    bool write_jpeg(const struct v4l2_buffer &bufferinfo)
    {
        char jpeg_file_name[PATH_MAX];

        snprintf(jpeg_file_name, sizeof(jpeg_file_name) - 1, "%s/image_%i.jpeg", (*options)["dir"].as<std::string>().c_str(), frames_taken);

        std::fstream result(jpeg_file_name, std::ios::binary | std::ios::out | std::ios::trunc);
        if (result.fail()) {
            LOG(ERROR) << "open file failed: " << strerror(errno);
            return false;
        }

        try {
            result.write(static_cast<const char*>(buffer.start), bufferinfo.length);
        } catch (std::exception &exc) {
            LOG(ERROR) << "write file failed: " << exc.what();
            return false;
        }

        frames_taken++;

        return true;
    }
};

int
main(int argc, char** argv)
{
    auto options = std::make_shared<cxxopts::Options>("uvccapture2", "Capture images from an USB camera on Linux");

    // clang-format off
    options->add_options()
        ("h,help", "show this help")
        ("debug", "enable debugging")
        ("dir", "directory where to save images", cxxopts::value<std::string>())
        ("device", "camera's device ti use", cxxopts::value<std::string>()->default_value("/dev/video0"))
        ("resolution", "image's resolution", cxxopts::value<std::string>()->default_value("640x480"))
        ("skip", "skip specified number of frames before first capture", cxxopts::value<int>())
        ("count", "number of images to capture", cxxopts::value<int>())
        ("pause", "pause between subsequent captures in seconds", cxxopts::value<double>())
        ("loop", "run in loop mode, overrides --count", cxxopts::value<bool>())
        ;
    // clang-format on

    options->parse(argc, argv);

    if (options->count("help")) {
        std::cout << options->help() << std::endl;
        return 0;
    }

    if (options->count("dir") == 0) {
        std::cout << "Mandatory parameter '--dir' was not specified." << std::endl;
        return 0;
    }

    el::Configurations defaultConf;
    defaultConf.setToDefault();
    // Values are always std::string
    defaultConf.set(el::Level::Info, el::ConfigurationType::Format, "%datetime %level %loc %msg");
    defaultConf.set(el::Level::Error, el::ConfigurationType::Format, "%datetime %level %loc %msg");
    // default logger uses default configurations
    el::Loggers::reconfigureLogger("default", defaultConf);

    std::cout << "images' directory: " << (*options)["dir"].as<std::string>() << std::endl;
    std::cout << "device: " << (*options)["device"].as<std::string>() << std::endl;
    std::cout << "loop: " << (*options)["loop"].as<bool>() << std::endl;

    V4L2Device dev(options);
    auto ok = dev.initialize();

    if (ok) {
        ok = dev.capture();
    }

    return (ok ? EXIT_SUCCESS : EXIT_FAILURE);
}
