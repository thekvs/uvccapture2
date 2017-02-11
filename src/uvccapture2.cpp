#include <fcntl.h>
#include <setjmp.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <linux/limits.h>
#include <linux/types.h>
#include <linux/videodev2.h>

#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <jpeglib.h>

#include "cxxopts/cxxopts.hpp"
#include "easylogging++/easylogging++.h"

using OptionsPtr = std::shared_ptr<cxxopts::Options>;
using MemBufferPtr = std::unique_ptr<unsigned char[]>;
using EpollEventPtr = std::unique_ptr<struct epoll_event>;

static const int kDefaultJPEGQuality = 75;
static const int kBuffersCount = 16 * 2;

INITIALIZE_EASYLOGGINGPP

struct JPEGErrorManager {
    /* "public" fields */
    struct jpeg_error_mgr pub;
    OptionsPtr options;
    /* for return to caller */
    jmp_buf setjmp_buffer;
};

char jpeg_last_error_msg[JMSG_LENGTH_MAX];

void
jpeg_error_exit_cb(j_common_ptr cinfo)
{
    // cinfo->err actually points to a JPEGErrorManager struct
    JPEGErrorManager* myerr = (JPEGErrorManager*)cinfo->err;
    // note : *(cinfo->err) is now equivalent to myerr->pub

    // output_message is a method to print an error message
    // (* (cinfo->err->output_message) ) (cinfo);

    // Create the message
    myerr->pub.format_message(cinfo, jpeg_last_error_msg);

    // Jump to the setjmp point
    longjmp(myerr->setjmp_buffer, 1);
}

void
jpeg_output_message_cb(j_common_ptr cinfo)
{
    JPEGErrorManager* error_manager = (JPEGErrorManager*)cinfo->err;

    auto quiet = (*(error_manager->options))["quiet"].as<bool>();
    if (not quiet) {
        error_manager->pub.format_message(cinfo, jpeg_last_error_msg);
        LOG(WARNING) << jpeg_last_error_msg;
    }
}

class V4L2Device
{
public:
    V4L2Device() = delete;

    V4L2Device(OptionsPtr opts)
        : options(opts)
    {
    }

    ~V4L2Device()
    {
        if (fd != -1) {
            close(fd);
        }
    }

    bool
    initialize()
    {
        auto initialized = open_device() and check_capabilities() and set_format() and init_buffers();

        return initialized;
    }

    bool
    capture()
    {
        struct v4l2_buffer bufferinfo;
        bool status = true;

        for (int i = 0; i < kBuffersCount; i++) {
            std::memset(&bufferinfo, 0, sizeof(bufferinfo));

            bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            bufferinfo.memory = V4L2_MEMORY_MMAP;
            bufferinfo.index = i; /* Queueing buffer index i. */

            // Put the buffer in the incoming queue.
            if (ioctl(fd, VIDIOC_QBUF, &bufferinfo) < 0) {
                LOG(ERROR) << "VIDIOC_QBUF failed: " << strerror(errno);
                return false;
            }
        }

        // Activate streaming
        int type = bufferinfo.type;
        if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
            LOG(ERROR) << "VIDIOC_STREAMON failed: " << strerror(errno);
            return false;
        }

        auto efd = epoll_create(100);
        if (efd == -1) {
            LOG(ERROR) << "epoll_create() failed: " << strerror(errno);
            return false;
        }

        struct epoll_event event;
        std::memset(&event, 0, sizeof(event));

        event.data.fd = fd;
        event.events = EPOLLIN | EPOLLET;

        auto rc = epoll_ctl(efd, EPOLL_CTL_ADD, fd, &event);
        if (rc == -1) {
            LOG(ERROR) << "epoll_ctl() failed: " << strerror(errno);
            return false;
        }

        EpollEventPtr events = EpollEventPtr(new struct epoll_event);

        int frames_skipped = 0;

        auto loop = (*options)["loop"].as<bool>();
        auto ignore_jpeg_errors = (*options)["ignore-jpeg-errors"].as<bool>();
        int frames_count = options->count("count") ? (*options)["count"].as<int>() : 1;
        int frames_to_skip = options->count("skip") ? (*options)["skip"].as<int>() : 0;
        useconds_t pause = std::lround((options->count("pause") ? (*options)["pause"].as<double>() : 0) * 1e6);

        while ((frames_taken < frames_count) or loop) {
            auto rc = epoll_wait(efd, events.get(), 1, -1);
            if (rc == -1) {
                LOG(ERROR) << "epoll_wait() error: " << strerror(errno);
                status = false;
                break;
            }

            if (rc == 0) {
                LOG(WARNING) << "epoll_wait() returned 0";
                continue;
            }

            if ((events->events & EPOLLERR) or (events->events & EPOLLHUP) or (!(events->events & EPOLLIN))) {
                LOG(ERROR) << "epoll error";
                status = false;
                break;
            }

            // Dequeue the buffer.
            if (ioctl(fd, VIDIOC_DQBUF, &bufferinfo) < 0) {
                LOG(ERROR) << "VIDIOC_QBUF failed: " << strerror(errno);
                status = false;
                break;
            }

            bool skip_frame = frames_to_skip > 0 and frames_skipped < frames_to_skip;
            bool ok = false;

            if (not skip_frame) {
                ok = write_jpeg(bufferinfo);
                if (not ok) {
                    if (not ignore_jpeg_errors) {
                        status = false;
                        break;
                    }
                } else {
                    frames_taken++;
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

            if (pause > 0 and (not skip_frame) and ok) {
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

        void* start = nullptr;
        size_t size = 0;
    };

    class RawImage
    {
    public:
        RawImage() = default;
        RawImage(RawImage&) = delete;

        RawImage(RawImage&& other)
            : raw_data(std::move(other.raw_data))
            , width(other.width)
            , height(other.height)
        {
            other.width = 0;
            other.height = 0;
        }

        MemBufferPtr raw_data;

        unsigned int width = 0;
        unsigned int height = 0;
    };

    using RawImagePtr = std::unique_ptr<RawImage>;

    int fd = -1;
    int frames_taken = 0;

    std::array<IOBuffer, kBuffersCount> buffers;

    OptionsPtr options;

    bool
    open_device()
    {
        if (fd == -1) {
            auto device = (*options)["device"].as<std::string>().c_str();
            fd = open(device, O_RDWR);
            if (fd < 0) {
                LOG(ERROR) << "Couldn't open '" << device << "': " << strerror(errno);
                return false;
            }
        } else {
            LOG(WARNING) << "Is device already initialized?";
            return false;
        }

        return true;
    }

    bool
    check_capabilities()
    {
        struct v4l2_capability cap;
        std::memset(&cap, 0, sizeof(cap));

        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
            LOG(ERROR) << "VIDIOC_QUERYCAP failed: " << strerror(errno);
            return false;
        }

        if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
            LOG(ERROR) << "The device does not handle single-planar video capture";
            return false;
        }

        if ((cap.capabilities & V4L2_CAP_STREAMING) == 0) {
            LOG(ERROR) << "The device does not handle frame streaming";
            return false;
        }

        return true;
    }

    bool
    set_format()
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

    bool
    init_buffers()
    {
        struct v4l2_requestbuffers bufrequest;
        std::memset(&bufrequest, 0, sizeof(bufrequest));

        bufrequest.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        bufrequest.memory = V4L2_MEMORY_MMAP;
        bufrequest.count = kBuffersCount;

        if (ioctl(fd, VIDIOC_REQBUFS, &bufrequest) < 0) {
            LOG(ERROR) << "VIDIOC_REQBUFS failed: " << strerror(errno);
            return false;
        }

        struct v4l2_buffer bufferinfo;

        for (int i = 0; i < kBuffersCount; i++) {
            std::memset(&bufferinfo, 0, sizeof(bufferinfo));

            bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            bufferinfo.memory = V4L2_MEMORY_MMAP;
            bufferinfo.index = i;

            if (ioctl(fd, VIDIOC_QUERYBUF, &bufferinfo) < 0) {
                LOG(ERROR) << "VIDIOC_QUERYBUF failed: " << strerror(errno);
                return false;
            }

            buffers[i].start = mmap(NULL, bufferinfo.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, bufferinfo.m.offset);
            if (buffers[i].start == MAP_FAILED) {
                LOG(ERROR) << "mmaping buffer failed: " << strerror(errno);
                return false;
            }

            buffers[i].size = bufferinfo.length;
        }

        return true;
    }

    std::tuple<bool, uint32_t, uint32_t>
    parse_resolution()
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
            } catch (std::invalid_argument& exc) {
                LOG(ERROR) << "no conversion could be performed: " << exc.what();
                ok = false;
            } catch (std::out_of_range& exc) {
                LOG(ERROR) << "out of range error: " << exc.what();
                ok = false;
            }
        } else {
            LOG(ERROR) << "invalid resolution description: " << resolution;
            ok = false;
        }

        return std::make_tuple(ok, x, y);
    }

    std::string
    make_jpeg_file_name()
    {
        char name[PATH_MAX];
        int rc = 0;

        auto tmpl = (*options)["result"].as<std::string>();

        auto use_strftime = (*options)["strftime"].as<bool>();
        if (use_strftime) {
            struct tm lt;
            auto t = std::time(nullptr);
            if (localtime_r(&t, &lt) == nullptr) {
                LOG(ERROR) << "localtime_r() failed";
            }
            rc = strftime(name, sizeof(name) - 1, tmpl.c_str(), &lt);
        } else {
            rc = snprintf(name, sizeof(name) - 1, tmpl.c_str(), frames_taken);
        }

        return (rc > 0 ? std::string(name) : std::string());
    }

    bool
    write_jpeg(const struct v4l2_buffer& bufferinfo)
    {
        auto idx = bufferinfo.index;

        auto jpeg_file_name = make_jpeg_file_name();
        if (jpeg_file_name.empty()) {
            LOG(ERROR) << "couldn't create result file name";
            return false;
        }

        auto save_jpeg_asis = (*options)["save-jpeg-asis"].as<bool>();
        if (save_jpeg_asis) { // store jpeg as we have received it from the camera
            std::fstream result(jpeg_file_name, std::ios::binary | std::ios::out | std::ios::trunc);
            if (result.fail()) {
                LOG(ERROR) << "open file failed: " << strerror(errno);
                return false;
            }

            try {
                result.write(static_cast<const char*>(buffers[idx].start), bufferinfo.length);
            } catch (std::exception& exc) {
                LOG(ERROR) << "write file failed: " << exc.what();
                return false;
            }

            return true;
        }

        // (Re)compress JPEG

        bool ok;
        RawImagePtr image;

        try {
            std::tie(ok, image) = decompress_jpeg(bufferinfo);
            if (not ok) {
                LOG(ERROR) << "image decompression failed!";
                return false;
            }

            ok = compress_jpeg(image, jpeg_file_name);
            if (not ok) {
                LOG(ERROR) << "image compression failed!";
                return false;
            }
        } catch (std::exception& exc) {
            LOG(WARNING) << "image (de)compression failed: " << exc.what();
            return false;
        }

        return true;
    }

    std::tuple<bool, RawImagePtr>
    decompress_jpeg(const struct v4l2_buffer& bufferinfo)
    {
        struct jpeg_decompress_struct cinfo;
        RawImagePtr image;

        JPEGErrorManager jerr;
        jerr.options = options;
        cinfo.err = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit = jpeg_error_exit_cb;
        jerr.pub.output_message = jpeg_output_message_cb;

        if (setjmp(jerr.setjmp_buffer)) {
            // If we get here, the JPEG code has signaled an error.
            auto quiet = (*options)["quiet"].as<bool>();
            if (not quiet) {
                LOG(WARNING) << jpeg_last_error_msg;
            }

            jpeg_destroy_decompress(&cinfo);

            return std::make_tuple(false, std::move(image));
        }

        jpeg_create_decompress(&cinfo);

        auto idx = bufferinfo.index;
        jpeg_mem_src(&cinfo, static_cast<unsigned char*>(buffers[idx].start), bufferinfo.length);

        auto rc = jpeg_read_header(&cinfo, TRUE);
        if (rc != 1) {
            LOG(ERROR) << "broken JPEG";
            return std::make_tuple(false, std::move(image));
        }

        jpeg_start_decompress(&cinfo);

        auto width = cinfo.output_width;
        auto height = cinfo.output_height;
        auto pixel_size = cinfo.output_components;
        auto row_stride = width * pixel_size;
        auto raw_size = width * height * pixel_size;

        image = RawImagePtr(new RawImage);

        image->raw_data = MemBufferPtr(new unsigned char[raw_size]);
        image->width = width;
        image->height = height;

        while (cinfo.output_scanline < cinfo.output_height) {
            unsigned char* buffer_array[1];
            buffer_array[0] = image->raw_data.get() + (cinfo.output_scanline) * row_stride;
            jpeg_read_scanlines(&cinfo, buffer_array, 1);
        }

        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);

        return std::make_tuple(true, std::move(image));
    }

    bool
    compress_jpeg(const RawImagePtr& image, const std::string& jpeg_file_name)
    {
        struct jpeg_compress_struct cinfo;
        struct jpeg_error_mgr jerr;

        JSAMPROW row_pointer[1];

        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_compress(&cinfo);

        FILE* outfile = fopen(jpeg_file_name.c_str(), "wb");
        if (outfile == nullptr) {
            LOG(ERROR) << "can't open '" << jpeg_file_name << "': " << strerror(errno);
            return false;
        }

        jpeg_stdio_dest(&cinfo, outfile);

        cinfo.image_width = image->width; /* image width and height, in pixels */
        cinfo.image_height = image->height;
        cinfo.input_components = 3; /* # of color components per pixel */
        cinfo.in_color_space = JCS_RGB; /* colorspace of input image */

        jpeg_set_defaults(&cinfo);

        int quality = kDefaultJPEGQuality;
        if (options->count("quality")) {
            quality = (*options)["quality"].as<int>();
        }

        jpeg_set_quality(&cinfo, quality, TRUE /* limit to baseline-JPEG values */);
        jpeg_start_compress(&cinfo, TRUE);

        auto row_stride = image->width * 3; /* JSAMPLEs per row in image_buffer */
        while (cinfo.next_scanline < cinfo.image_height) {
            row_pointer[0] = &(image->raw_data.get()[cinfo.next_scanline * row_stride]);
            jpeg_write_scanlines(&cinfo, row_pointer, 1);
        }

        jpeg_finish_compress(&cinfo);
        fclose(outfile);
        jpeg_destroy_compress(&cinfo);

        return true;
    }
};

int
main(int argc, char** argv)
{
    auto options = std::make_shared<cxxopts::Options>("uvccapture2", "Capture images from an USB camera on Linux");

    // clang-format off
    options->add_options()
        ("h,help", "show this help and exit")
        ("result", "jpeg image name template", cxxopts::value<std::string>())
        ("device", "camera's device device use", cxxopts::value<std::string>()->default_value("/dev/video0"))
        ("resolution", "image's resolution", cxxopts::value<std::string>()->default_value("640x480"))
        ("quality", "compression quality for jpeg file (default: 75)", cxxopts::value<int>())
        ("skip", "skip specified number of frames before first capture", cxxopts::value<int>())
        ("count", "number of images to capture", cxxopts::value<int>())
        ("pause", "pause between subsequent captures in seconds", cxxopts::value<double>())
        ("loop", "run in a loop mode, overrides --count", cxxopts::value<bool>())
        ("strftime", "expand the filename with date and time information", cxxopts::value<bool>())
        ("save-jpeg-asis", "store jpeg as we have received it from an USB camera", cxxopts::value<bool>())
        ("ignore-jpeg-errors", "ignore libjpeg errors", cxxopts::value<bool>())
        ("quiet", "do not show errors and warnings from libjpeg", cxxopts::value<bool>())
        ;
    // clang-format on

    options->parse(argc, argv);

    if (options->count("help")) {
        std::cout << options->help() << std::endl;
        return EXIT_SUCCESS;
    }

    el::Configurations el_config;
    el_config.setToDefault();

// Values are always std::string
#ifndef NDEBUG
    el_config.set(el::Level::Info, el::ConfigurationType::Format, "%datetime %level %loc %msg");
    el_config.set(el::Level::Error, el::ConfigurationType::Format, "%datetime %level %loc %msg");
    el_config.set(el::Level::Warning, el::ConfigurationType::Format, "%datetime %level %loc %msg");
#else
    el_config.set(el::Level::Info, el::ConfigurationType::Format, "%level %msg");
    el_config.set(el::Level::Error, el::ConfigurationType::Format, "%level %msg");
    el_config.set(el::Level::Warning, el::ConfigurationType::Format, "%level %msg");
#endif
    // do not log to file
    el_config.setGlobally(el::ConfigurationType::ToFile, "false");

    // default logger uses default configurations
    el::Loggers::reconfigureLogger("default", el_config);

    if (options->count("quality")) {
        auto quality = (*options)["quality"].as<int>();
        if (quality < 0 or quality > 100) {
            LOG(ERROR) << "inavalid value for '--quality' parameter, has to be between 0 and 100.";
            return EXIT_FAILURE;
        }
    }

    if (options->count("result") == 0) {
        LOG(ERROR) << "Mandatory parameter '--result' was not specified.";
        return EXIT_FAILURE;
    }

    V4L2Device dev(options);
    auto ok = dev.initialize();

    if (ok) {
        ok = dev.capture();
    }

    return (ok ? EXIT_SUCCESS : EXIT_FAILURE);
}
