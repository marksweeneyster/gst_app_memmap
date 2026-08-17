#include "shm_frame.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <string_view>

namespace {

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  std::atomic<bool> g_running{true};

  /// Shared memory context for the video reader/consumer
  struct reader_context_t {
    shm::shared_region_t region;
    shm::semaphore_pair_t sems;
    std::uint64_t index = 0;
    bool caps_set       = false;
    bool verbose        = false;
  };

  void on_signal(int /*unused*/) {
    g_running.store(false, std::memory_order_relaxed);
  }

  void print_help(std::string_view app_name) {
    std::cout << app_name << " optional arguments:\n";
    std::cout
            << "  --sink <sink_name> : output sink, default is autovideosink\n";
    std::cout << "  --verbose          : print frame info , default is off\n";
  }

}// namespace

// NOLINTNEXTLINE(modernize-use-trailing-return-type)
int main(int argc, char** argv) {
  gst_init(&argc, &argv);

  const char* sink_name = "autovideosink";
  bool verbose          = false;
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    ::print_help(argv[0]);
    return 0;
  }
  for (int arg_index = 1; arg_index < argc; ++arg_index) {
    if (std::strcmp(argv[arg_index], "--sink") == 0 && arg_index + 1 < argc) {
      sink_name = argv[++arg_index];
    } else if (std::strcmp(argv[arg_index], "--verbose") == 0) {
      verbose = true;
    }
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

  struct sigaction action{};
  action.sa_handler = ::on_signal;

  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);

  ::reader_context_t ctx;
  ctx.verbose = verbose;
  try {
    ctx.region = shm::attach_region(g_running);
    ctx.sems   = shm::attach_semaphore_pair(g_running);
  } catch (const std::exception& e) {
    std::cerr << "reader: " << e.what() << '\n';
    return 1;
  }

  GstElement* pipeline      = gst_pipeline_new("reader");
  GstElement* appsrc        = gst_element_factory_make("appsrc", nullptr);
  GstElement* queue         = gst_element_factory_make("queue", nullptr);
  GstElement const* convert = gst_element_factory_make("videoconvert", nullptr);
  GstElement const* sink    = gst_element_factory_make(sink_name, nullptr);

  if (appsrc == nullptr || queue == nullptr || convert == nullptr ||
      sink == nullptr) {
    std::cout << "reader: failed to create pipeline elements (sink="
              << sink_name << ")\n";
    gst_object_unref(pipeline);
    return 1;
  }

  gst_app_src_set_stream_type(GST_APP_SRC(appsrc), GST_APP_STREAM_TYPE_STREAM);
  gst_app_src_set_max_buffers(GST_APP_SRC(appsrc), 2);
  // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
  g_object_set(appsrc, "is-live", TRUE, "do-timestamp", TRUE, "format",
               GST_FORMAT_TIME, nullptr);

  gst_bin_add_many(GST_BIN(pipeline), appsrc, queue, convert, sink, nullptr);
  gst_element_link_many(appsrc, queue, convert, sink, nullptr);
  // NOLINTEND(cppcoreguidelines-pro-type-vararg)

  gst_element_set_state(pipeline, GST_STATE_PLAYING);

  std::cout << "\nreader: starting loop\n\n";
  try {
    while (g_running.load(std::memory_order_relaxed)) {
      if (!shm::wait_sem_or_stop(ctx.sems.data_sem.get(), g_running)) {
        break;
      }

      if (!ctx.caps_set) {
        GstCaps* caps = shm::make_caps(*shm::header(ctx.region.base()));
        if (caps == nullptr) {
          std::cerr << "reader: cannot build caps from header\n";
          break;
        }
        gst_app_src_set_caps(GST_APP_SRC(appsrc), caps);
        gst_caps_unref(caps);
        ctx.caps_set = true;
      }

      const std::size_t slot  = ctx.index % shm::k_slot_count;
      const std::uint8_t* src = shm::slot_base(ctx.region.base(), slot);
      const std::uint64_t seq = shm::header(ctx.region.base())->seq;

      GstBuffer* buffer =
              gst_buffer_new_allocate(nullptr, shm::k_frame_size, nullptr);
      GstMapInfo map;
      if (gst_buffer_map(buffer, &map, GST_MAP_WRITE) == FALSE) {
        gst_buffer_unref(buffer);
        std::cerr << "reader: gst_buffer_map failed\n";
        break;
      }
      std::memcpy(map.data, src, shm::k_frame_size);
      gst_buffer_unmap(buffer, &map);

      ctx.sems.free_sem.post();

      if (ctx.verbose) {
        std::cerr << "reader: consumed seq=" << seq << " slot=" << slot << '\n';
      }

      const GstFlowReturn ret =
              gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
      if (ret != GST_FLOW_OK) {
        std::cerr << "reader: push_buffer returned " << gst_flow_get_name(ret)
                  << '\n';
        break;
      }
      ctx.index++;
    }
  } catch (const std::exception& e) {
    std::cerr << "reader: " << e.what() << '\n';
  }

  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  return 0;
}
