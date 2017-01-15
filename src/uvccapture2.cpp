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
        auto ok = open_device() and check_capabilities() and set_format() and init_buffers();
        if (not ok) {
            return false;
        }

        return true;
    }

    void capture()
    {
        struct v4l2_buffer bufferinfo;
        std::memset(&bufferinfo, 0, sizeof(bufferinfo));

        bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        bufferinfo.memory = V4L2_MEMORY_MMAP;
        bufferinfo.index = 0; /* Queueing buffer index 0. */

        // Put the buffer in the incoming queue.
        if (ioctl(fd, VIDIOC_QBUF, &bufferinfo) < 0) {
            LOG(ERROR) << "VIDIOC_QBUF failed: " << strerror(errno);
            return;
        }

        // Activate streaming
        int type = bufferinfo.type;
        if (ioctl(fd, VIDIOC_STREAMON, &type) < 0){
            LOG(ERROR) << "VIDIOC_STREAMON failed: " << strerror(errno);
            return;
        }

        int images_taken = 0;
        auto images_count = (*options)["count"].as<int>();

        char fname[PATH_MAX];
        auto dir = (*options)["dir"].as<std::string>();

        while (images_taken < images_count) {
            // Dequeue the buffer.
            if (ioctl(fd, VIDIOC_DQBUF, &bufferinfo) < 0) {
                LOG(ERROR) << "VIDIOC_QBUF failed: " << strerror(errno);
                break;
            }

            snprintf(fname, sizeof(fname) - 1, "%s/image_%i.jpeg", dir.c_str(), images_taken);

            int jpgfile;
            if ((jpgfile = open(fname, O_WRONLY | O_CREAT, 0660)) < 0) {
                LOG(ERROR) << "open file failed: " << strerror(errno);
                break;
            }

            auto rc = write(jpgfile, buffer.start, bufferinfo.length);
            if (rc < 0) {
                LOG(ERROR) << "write file failed: " << strerror(errno);
                break;
            }

            close(jpgfile);

            bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            bufferinfo.memory = V4L2_MEMORY_MMAP;
            /* Set the index if using several buffers */

            // Queue the next one.
            if (ioctl(fd, VIDIOC_QBUF, &bufferinfo) < 0) {
                LOG(ERROR) << "VIDIOC_QBUF failed: " << strerror(errno);
                break;
            }

            images_taken++;
        }

        // Deactivate streaming
        if (ioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
            LOG(ERROR) << "VIDIOC_STREAMOFF failed: " << strerror(errno);
        }
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
            LOG(ERROR) << "the device does not to handle frame streaming";
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
        // FIXME: set format specified in cmd. line
        format.fmt.pix.width = 800;
        format.fmt.pix.height = 600;

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
        memset(buffer.start, 0, bufferinfo.length);

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
        ("dir", "name of an images directory", cxxopts::value<std::string>())
        ("device", "camera's device ti use", cxxopts::value<std::string>()->default_value("/dev/video0"))
        ("count", "number of images to capture", cxxopts::value<int>())
        ("resolution", "image's resolution", cxxopts::value<std::string>()->default_value("640x480"))
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

    V4L2Device dev(options);

    if (dev.initialize()) {
        dev.capture();
    }

    return 0;
}
