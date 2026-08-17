#include "shm_frame.hpp"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>

namespace {

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  std::atomic<bool> g_running{true};

  constexpr std::uint32_t k_poll_interval_us = 50'000;

  /// Shared memory context for the video writer/producer
  struct writer_context_t {
    shm::shared_region_t region;
    shm::semaphore_pair_t sems;
    std::uint64_t index = 0;
  };

  auto on_new_sample_impl(GstAppSink* sink, gpointer user_data)
          -> GstFlowReturn;

  auto on_new_sample(GstAppSink* sink, gpointer user_data) -> GstFlowReturn {
    try {
      return on_new_sample_impl(sink, user_data);
    } catch (const std::exception& e) {
      std::cerr << "writer: " << e.what() << '\n';
      return GST_FLOW_ERROR;
    }
  }

  auto on_new_sample_impl(GstAppSink* sink, gpointer user_data)
          -> GstFlowReturn {
    auto* ctx         = static_cast<writer_context_t*>(user_data);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (sample == nullptr) {
      return GST_FLOW_EOS;
    }
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ) == FALSE) {
      gst_sample_unref(sample);
      return GST_FLOW_ERROR;
    }
    if (map.size != shm::k_frame_size) {
      gst_buffer_unmap(buffer, &map);
      gst_sample_unref(sample);
      return GST_FLOW_ERROR;
    }

    if (!shm::wait_sem_or_stop(ctx->sems.free_sem.get(), g_running)) {
      gst_buffer_unmap(buffer, &map);
      gst_sample_unref(sample);
      return GST_FLOW_ERROR;
    }

    const std::size_t slot = ctx->index % shm::k_slot_count;
    std::uint8_t* dst      = shm::slot_base(ctx->region.base(), slot);
    std::memcpy(dst, map.data, map.size);
    shm::fill_header(*shm::header(ctx->region.base()), ctx->index);
    std::atomic_thread_fence(std::memory_order_release);
    ctx->sems.data_sem.post();
    ctx->index++;

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  GstAppSinkCallbacks kSinkCallbacks = {
          .eos         = nullptr,
          .new_preroll = nullptr,
          .new_sample  = on_new_sample,
          .new_event   = nullptr,
  };

  void on_signal(int /*unused*/) {
    g_running.store(false, std::memory_order_relaxed);
  }

}// namespace

// NOLINTNEXTLINE(modernize-use-trailing-return-type)
int main(int argc, char** argv) {
  gst_init(&argc, &argv);

  // NOLINTBEGIN(misc-include-cleaner)
  struct sigaction action{};
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
  // NOLINTEND(misc-include-cleaner)

  writer_context_t ctx;
  try {
    ctx.region = shm::create_region();
    std::memset(ctx.region.base(), 0, ctx.region.size());
    ctx.sems = shm::create_semaphore_pair();
  } catch (const std::exception& e) {
    std::cerr << "writer: " << e.what() << '\n';
    return 1;
  }

  GstElement* pipeline    = gst_pipeline_new("writer");
  GstElement* src         = gst_element_factory_make("videotestsrc", nullptr);
  GstElement* tunnel      = gst_element_factory_make("tunnel", nullptr);
  GstElement* convert     = gst_element_factory_make("videoconvert", nullptr);
  GstElement const* scale = gst_element_factory_make("videoscale", nullptr);
  GstElement* caps_filter = gst_element_factory_make("capsfilter", nullptr);
  GstElement* appsink     = gst_element_factory_make("appsink", nullptr);
  if (src == nullptr || tunnel == nullptr || convert == nullptr ||
      scale == nullptr || caps_filter == nullptr || appsink == nullptr) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    std::fprintf(stderr, "writer: failed to create pipeline elements\n");
    gst_object_unref(pipeline);
    return 1;
  }

  constexpr int colors_prop{24};
  constexpr int horizontal_speed{10};
  // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
  g_object_set(src, "pattern", colors_prop, nullptr);
  g_object_set(src, "horizontal-speed", horizontal_speed, nullptr);
  // NOLINTEND(cppcoreguidelines-pro-type-vararg)

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  GstCaps* filter_caps = gst_caps_new_simple(
          "video/x-raw", "format", G_TYPE_STRING, "I420", "width", G_TYPE_INT,
          static_cast<gint>(shm::k_width), "height", G_TYPE_INT,
          static_cast<gint>(shm::k_height), "framerate", GST_TYPE_FRACTION,
          static_cast<gint>(shm::k_framerate_n),
          static_cast<gint>(shm::k_framerate_d), nullptr);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  g_object_set(caps_filter, "caps", filter_caps, nullptr);
  gst_caps_unref(filter_caps);

  // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
  gst_bin_add_many(GST_BIN(pipeline), src, tunnel, convert, scale, caps_filter,
                   appsink, nullptr);
  gst_element_link_many(src, tunnel, convert, scale, caps_filter, appsink,
                        nullptr);
  // NOLINTEND(cppcoreguidelines-pro-type-vararg)

  gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &kSinkCallbacks, &ctx,
                             nullptr);

  gst_element_set_state(pipeline, GST_STATE_PLAYING);

  while (g_running.load(std::memory_order_relaxed)) {
    g_usleep(k_poll_interval_us);
  }

  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  return 0;
}
