#pragma once

#include <gst/gst.h>
#include <gst/video/video.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shm {

  constexpr std::uint32_t k_magic   = GST_MAKE_FOURCC('G', 'S', 'T', 'B');
  constexpr std::uint32_t k_version = 1;

  constexpr std::uint32_t k_width       = 1'920;
  constexpr std::uint32_t k_height      = 1'080;
  constexpr std::uint32_t k_framerate_n = 30;
  constexpr std::uint32_t k_framerate_d = 1;
  constexpr std::uint32_t k_fourcc_i420 = GST_MAKE_FOURCC('I', '4', '2', '0');
  constexpr std::size_t k_plane_count   = 3;
  constexpr std::size_t k_slot_count    = 2;
  constexpr std::size_t k_page_size     = 4'096;
  constexpr std::uint32_t k_perm_mode   = 0666;
  constexpr std::uint32_t k_attach_retry_ms  = 50;
  constexpr std::uint32_t k_sem_wait_step_ms = 100;
  constexpr std::uint64_t k_ns_per_ms        = 1'000'000ULL;
  constexpr std::uint64_t k_ns_per_sec       = 1'000'000'000ULL;

  constexpr const char* k_shm_name      = "/gst_shm_frame_broker";
  constexpr const char* k_sem_free_name = "/gst_shm_broker_free";
  constexpr const char* k_sem_data_name = "/gst_shm_broker_data";

  struct plane_info_t {
    std::uint32_t offset;
    std::uint32_t stride;
    std::uint32_t size;
  };

  struct frame_header_t {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t framerate_n;
    std::uint32_t framerate_d;
    std::uint32_t fourcc;
    std::uint32_t plane_count;
    std::array<plane_info_t, k_plane_count> planes;
    std::uint64_t seq;
  };

  constexpr auto i420_frame_size(std::uint32_t width, std::uint32_t height)
          -> std::size_t {
    const std::size_t luma_size = static_cast<std::size_t>(width) * height;
    const std::size_t uv_size =
            static_cast<std::size_t>(width + 1) / 2 * ((height + 1) / 2);
    return luma_size + (uv_size * 2);
  }

  constexpr auto align_up(std::size_t value, std::size_t alignment)
          -> std::size_t {
    return (value + alignment - 1) / alignment * alignment;
  }

  constexpr std::size_t k_frame_size = i420_frame_size(k_width, k_height);
  constexpr std::size_t k_header_size =
          align_up(sizeof(frame_header_t), k_page_size);
  constexpr std::size_t k_region_size =
          k_header_size + (k_slot_count * k_frame_size);

  inline auto header(void* base) -> frame_header_t* {
    return static_cast<frame_header_t*>(base);
  }

  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  inline auto slot_base(void* base, std::size_t index) -> std::uint8_t* {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return static_cast<std::uint8_t*>(base) + k_header_size +
           (index * k_frame_size);
  }

  inline void fill_header(frame_header_t& header, std::uint64_t seq) {
    constexpr std::size_t uv_size =
            static_cast<std::size_t>(k_width + 1) / 2 * ((k_height + 1) / 2);
    header.magic       = k_magic;
    header.version     = k_version;
    header.width       = k_width;
    header.height      = k_height;
    header.framerate_n = k_framerate_n;
    header.framerate_d = k_framerate_d;
    header.fourcc      = k_fourcc_i420;
    header.plane_count = k_plane_count;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    header.planes[0] = {
            .offset = 0,
            .stride = k_width,
            .size   = static_cast<std::uint32_t>(k_width * k_height),
    };
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    header.planes[1] = {
            .offset = static_cast<std::uint32_t>(k_width * k_height),
            .stride = (k_width + 1) / 2,
            .size   = static_cast<std::uint32_t>(uv_size),
    };
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    header.planes[2] = {
            .offset = static_cast<std::uint32_t>(
                    (static_cast<std::size_t>(k_width) * k_height) + uv_size),
            .stride = (k_width + 1) / 2,
            .size   = static_cast<std::uint32_t>(uv_size),
    };
    header.seq = seq;
  }

  inline auto make_caps(const frame_header_t& header) -> GstCaps* {
    const GstVideoFormat format = gst_video_format_from_fourcc(header.fourcc);
    const gchar* format_name    = gst_video_format_to_string(format);
    if (format_name == nullptr) {
      return nullptr;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return gst_caps_new_simple(
            "video/x-raw", "format", G_TYPE_STRING, format_name, "width",
            G_TYPE_INT, static_cast<gint>(header.width), "height", G_TYPE_INT,
            static_cast<gint>(header.height), "framerate", GST_TYPE_FRACTION,
            static_cast<gint>(header.framerate_n),
            static_cast<gint>(header.framerate_d), nullptr);
  }


  class shared_region_t {
  public:
    shared_region_t() = default;
    ~shared_region_t() { close_(); }

    shared_region_t(const shared_region_t&)                    = delete;
    auto operator=(const shared_region_t&) -> shared_region_t& = delete;

    shared_region_t(shared_region_t&& other) noexcept { move_from_(other); }
    auto operator=(shared_region_t&& other) noexcept -> shared_region_t& {
      if (this != &other) {
        close_();
        move_from_(other);
      }
      return *this;
    }

    static auto Create(const char* file_name, std::size_t size)
            -> shared_region_t {
      const int shm_fd =
              shm_open(file_name, O_CREAT | O_EXCL | O_RDWR, k_perm_mode);
      if (shm_fd < 0) {
        throw std::runtime_error(std::string("shm_open ") + file_name + ": " +
                                 std::strerror(errno));
      }
      if (ftruncate(shm_fd, static_cast<off_t>(size)) != 0) {
        const int saved = errno;
        close(shm_fd);
        shm_unlink(file_name);
        throw std::runtime_error(std::string("ftruncate ") + file_name + ": " +
                                 std::strerror(saved));
      }
      void* base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        shm_fd, 0);
      if (base == MAP_FAILED) {
        const int saved = errno;
        close(shm_fd);
        shm_unlink(file_name);
        throw std::runtime_error(std::string("mmap ") + file_name + ": " +
                                 std::strerror(saved));
      }
      shared_region_t region;
      region._fd   = shm_fd;
      region._base = base;
      region._size = size;
      region._owns = true;
      return region;
    }

    static auto Attach(const char* file_name, std::size_t size)
            -> shared_region_t {
      const int shm_fd = shm_open(file_name, O_RDWR, 0);
      if (shm_fd < 0) {
        throw std::runtime_error(std::string("shm_open ") + file_name + ": " +
                                 std::strerror(errno));
      }
      void* base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        shm_fd, 0);
      if (base == MAP_FAILED) {
        const int saved = errno;
        close(shm_fd);
        throw std::runtime_error(std::string("mmap ") + file_name + ": " +
                                 std::strerror(saved));
      }
      shared_region_t region;
      region._fd   = shm_fd;
      region._base = base;
      region._size = size;
      region._owns = false;
      return region;
    }

    [[nodiscard]] auto base() const -> void* { return _base; }
    [[nodiscard]] auto size() const -> std::size_t { return _size; }

  private:
    int _fd           = -1;
    void* _base       = MAP_FAILED;
    std::size_t _size = 0;
    bool _owns        = false;

    void move_from_(shared_region_t& other) noexcept {
      _fd         = other._fd;
      _base       = other._base;
      _size       = other._size;
      _owns       = other._owns;
      other._fd   = -1;
      other._base = MAP_FAILED;
      other._size = 0;
      other._owns = false;
    }

    void close_() {
      if (_base != MAP_FAILED) {
        munmap(_base, _size);
        _base = MAP_FAILED;
      }
      if (_fd >= 0) {
        close(_fd);
        _fd = -1;
      }
      if (_owns) {
        shm_unlink(k_shm_name);
        _owns = false;
      }
    }
  };

  class named_semaphore_t {
  public:
    named_semaphore_t() = default;
    ~named_semaphore_t() { close_(); }

    named_semaphore_t(const named_semaphore_t&)                    = delete;
    auto operator=(const named_semaphore_t&) -> named_semaphore_t& = delete;

    named_semaphore_t(named_semaphore_t&& other) noexcept { move_from_(other); }
    auto operator=(named_semaphore_t&& other) noexcept -> named_semaphore_t& {
      if (this != &other) {
        close_();
        move_from_(other);
      }
      return *this;
    }

    static auto Create(const char* name, unsigned value) -> named_semaphore_t {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      sem_t* sem = sem_open(name, O_CREAT | O_EXCL, k_perm_mode, value);
      if (sem == SEM_FAILED) {
        throw std::runtime_error(std::string("sem_open ") + name + ": " +
                                 std::strerror(errno));
      }
      named_semaphore_t sema;
      sema._name = name;
      sema._sem  = sem;
      sema._owns = true;
      return sema;
    }

    static auto Attach(const char* name) -> named_semaphore_t {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      sem_t* sem = sem_open(name, 0);
      if (sem == SEM_FAILED) {
        throw std::runtime_error(std::string("sem_open ") + name + ": " +
                                 std::strerror(errno));
      }
      named_semaphore_t sema;
      sema._name = name;
      sema._sem  = sem;
      sema._owns = false;
      return sema;
    }

    [[nodiscard]] auto get() const -> sem_t* { return _sem; }

    void post() const {
      if (sem_post(_sem) != 0) {
        throw std::runtime_error(std::string("sem_post ") + _name + ": " +
                                 std::strerror(errno));
      }
    }

  private:
    const char* _name = nullptr;
    sem_t* _sem       = SEM_FAILED;
    bool _owns        = false;

    void move_from_(named_semaphore_t& other) noexcept {
      _name       = other._name;
      _sem        = other._sem;
      _owns       = other._owns;
      other._name = nullptr;
      other._sem  = SEM_FAILED;
      other._owns = false;
    }

    void close_() {
      if (_sem != SEM_FAILED) {
        sem_close(_sem);
        _sem = SEM_FAILED;
      }
      if (_owns && _name != nullptr) {
        sem_unlink(_name);
        _owns = false;
      }
    }
  };

  struct semaphore_pair_t {
    named_semaphore_t free_sem;
    named_semaphore_t data_sem;
  };

  /// @brief For video writer/producers to create and get the semaphore used to sync with reader/consumers
  inline auto create_semaphore_pair() -> semaphore_pair_t {
    try {
      return semaphore_pair_t{
              .free_sem =
                      named_semaphore_t::Create(k_sem_free_name, k_slot_count),
              .data_sem = named_semaphore_t::Create(k_sem_data_name, 0),
      };
    } catch (const std::exception&) {
      sem_unlink(k_sem_free_name);
      sem_unlink(k_sem_data_name);
      return semaphore_pair_t{
              .free_sem =
                      named_semaphore_t::Create(k_sem_free_name, k_slot_count),
              .data_sem = named_semaphore_t::Create(k_sem_data_name, 0),
      };
    }
  }

  /// @brief For video writer/producers to create and get the shared memory region
  inline auto create_region() -> shared_region_t {
    try {
      return shared_region_t::Create(k_shm_name, k_region_size);
    } catch (const std::exception&) {
      shm_unlink(k_shm_name);
      return shared_region_t::Create(k_shm_name, k_region_size);
    }
  }

  /// @brief For video reader/consumers to get the shared memory region created by the writer
  inline auto attach_region(const std::atomic<bool>& running)
          -> shared_region_t {
    for (;;) {
      try {
        return shared_region_t::Attach(k_shm_name, k_region_size);
      } catch (const std::exception&) {
        if (!running.load(std::memory_order_relaxed)) {
          throw std::runtime_error("attach_region interrupted");
        }
        std::this_thread::sleep_for(
                std::chrono::milliseconds(k_attach_retry_ms));
      }
    }
  }

  /// @brief For video reader/consumers to get the semaphore pair created by the writer
  inline auto attach_semaphore_pair(const std::atomic<bool>& running)
          -> semaphore_pair_t {
    for (;;) {
      try {
        return semaphore_pair_t{
                .free_sem = named_semaphore_t::Attach(k_sem_free_name),
                .data_sem = named_semaphore_t::Attach(k_sem_data_name),
        };
      } catch (const std::exception&) {
        if (!running.load(std::memory_order_relaxed)) {
          throw std::runtime_error("attach_semaphore_pair interrupted");
        }
        std::this_thread::sleep_for(
                std::chrono::milliseconds(k_attach_retry_ms));
      }
    }
  }

  inline auto
  wait_sem_or_stop(sem_t* sem, const std::atomic<bool>& running,
                   std::chrono::milliseconds step = std::chrono::milliseconds(
                           k_sem_wait_step_ms)) -> bool {
    struct timespec tm_spec{};
    for (;;) {
      clock_gettime(CLOCK_REALTIME, &tm_spec);
      std::uint64_t nano_sec =
              static_cast<std::uint64_t>(tm_spec.tv_nsec) +
              (static_cast<std::uint64_t>(step.count()) * k_ns_per_ms);
      tm_spec.tv_sec += static_cast<time_t>(nano_sec / k_ns_per_sec);
      tm_spec.tv_nsec  = static_cast<std::int64_t>(nano_sec % k_ns_per_sec);
      const int rcount = sem_timedwait(sem, &tm_spec);
      if (rcount == 0) {
        return true;
      }
      if (errno == EINTR) {
        if (!running.load(std::memory_order_relaxed)) {
          return false;
        }
        continue;
      }
      if (errno != ETIMEDOUT) {
        throw std::runtime_error(std::string("sem_timedwait: ") +
                                 std::strerror(errno));
      }
      if (!running.load(std::memory_order_relaxed)) {
        return false;
      }
    }
  }

}// namespace shm
