/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2018 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/emulator_window.h"

#include <QVulkanWindow>

#include "third_party/imgui/imgui.h"
#include "xenia/apu/xaudio2/xaudio2_audio_system.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/hid/input_system.h"
#include "xenia/hid/xinput/xinput_hid.h"
#include "xenia/ui/vulkan/vulkan_instance.h"
#include "xenia/ui/vulkan/vulkan_provider.h"

DEFINE_string(apu, "any", "Audio system. Use: [any, nop, xaudio2]", "General");
DEFINE_string(gpu, "any", "Graphics system. Use: [any, vulkan, null]",
              "General");
DEFINE_string(hid, "any", "Input system. Use: [any, nop, winkey, xinput]",
              "General");

DEFINE_string(target, "", "Specifies the target .xex or .iso to execute.",
              "General");
DEFINE_bool(fullscreen, false, "Toggles fullscreen", "General");

namespace xe {
namespace app {

class VulkanWindow : public QVulkanWindow {
 public:
  VulkanWindow(gpu::vulkan::VulkanGraphicsSystem* gfx)
      : graphics_system_(gfx) {}
  QVulkanWindowRenderer* createRenderer() override;

 private:
  gpu::vulkan::VulkanGraphicsSystem* graphics_system_;
};

class VulkanRenderer : public QVulkanWindowRenderer {
 public:
  VulkanRenderer(VulkanWindow* window,
                 gpu::vulkan::VulkanGraphicsSystem* graphics_system)
      : window_(window), graphics_system_(graphics_system) {}

  void startNextFrame() override {
    // Copy the graphics frontbuffer to our backbuffer.
    //auto swap_state = graphics_system_->swap_state();

    auto cmd = window_->currentCommandBuffer();
    //auto src = reinterpret_cast<VkImage>(
    //    swap_state->buffer_textures[swap_state->current_buffer]);
    auto dest = window_->swapChainImage(window_->currentSwapChainImageIndex());
    auto dest_size = window_->swapChainImageSize();

    VkImageMemoryBarrier barrier;
    std::memset(&barrier, 0, sizeof(VkImageMemoryBarrier));
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    //barrier.image = src;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    VkImageBlit region;
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[0] = {0, 0, 0};
    /*region.srcOffsets[1] = {static_cast<int32_t>(swap_state->width),
                            static_cast<int32_t>(swap_state->height), 1};*/

    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[0] = {0, 0, 0};
    region.dstOffsets[1] = {static_cast<int32_t>(dest_size.width()),
                            static_cast<int32_t>(dest_size.height()), 1};
   /* vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, dest,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_LINEAR);*/

    //swap_state->pending = false;
    window_->frameReady();
  }

 private:
  gpu::vulkan::VulkanGraphicsSystem* graphics_system_;
  VulkanWindow* window_;
};

QVulkanWindowRenderer* VulkanWindow::createRenderer() {
  return new VulkanRenderer(this, graphics_system_);
}

EmulatorWindow::EmulatorWindow(Loop* loop, const std::string& title)
    : QtWindow(loop, title) {
  // TODO(DrChat): Pass in command line arguments.
  emulator_ = std::make_unique<xe::Emulator>("","","");

  auto audio_factory = [&](cpu::Processor* processor,
                           kernel::KernelState* kernel_state) {
    auto audio = apu::xaudio2::XAudio2AudioSystem::Create(processor);
    if (audio->Setup(kernel_state) != X_STATUS_SUCCESS) {
      audio->Shutdown();
      return std::unique_ptr<apu::AudioSystem>(nullptr);
    }

    return audio;
  };
  
  graphics_provider_ = ui::vulkan::VulkanProvider::Create(nullptr);
  auto graphics_factory = [&](cpu::Processor* processor,
                              kernel::KernelState* kernel_state) {
    auto graphics = std::make_unique<gpu::vulkan::VulkanGraphicsSystem>();
    if (graphics->Setup(processor, kernel_state, 
                        graphics_provider_->CreateOffscreenContext()->target_window())) {
      graphics->Shutdown();
      return std::unique_ptr<gpu::vulkan::VulkanGraphicsSystem>(nullptr);
    }

    return graphics;
  };

  auto input_factory = [&](ui::Window* window) {
    std::vector<std::unique_ptr<hid::InputDriver>> drivers;
    auto xinput_driver = hid::xinput::Create(window);
    xinput_driver->Setup();
    drivers.push_back(std::move(xinput_driver));

    return drivers;
  };

  //X_STATUS result = emulator_->Setup(this, audio_factory, graphics_factory, input_factory);
  //if (result == X_STATUS_SUCCESS) {
  //  // Setup a callback called when the emulator wants to swap.
  //  emulator_->graphics_system()->SetSwapCallback([&]() {
  //    QMetaObject::invokeMethod(this->graphics_window_.get(), "requestUpdate",
  //                              Qt::QueuedConnection);
  //  });
  //}

  //// Initialize our backend display window.
  //if (!InitializeVulkan()) {
  //  return;
  //}

  //// Set a callback on launch
  //emulator_->on_launch.AddListener([this]() {
  //  auto title_db = this->emulator()->game_data();
  //  if (title_db) {
  //    QPixmap p;
  //    auto icon_block = title_db->icon();
  //    if (icon_block.buffer &&
  //        p.loadFromData(icon_block.buffer, uint(icon_block.size), "PNG")) {
  //      this->setWindowIcon(QIcon(p));
  //    }
  //  }
  //});
}

bool EmulatorWindow::InitializeVulkan() {
  auto provider =
      reinterpret_cast<ui::vulkan::VulkanProvider*>(graphics_provider_.get());

  // Create a Qt wrapper around our vulkan instance.
  vulkan_instance_ = std::make_unique<QVulkanInstance>();
  vulkan_instance_->setVkInstance(*provider->instance());
  if (!vulkan_instance_->create()) {
    return false;
  }

  graphics_window_ = std::make_unique<VulkanWindow>(
      reinterpret_cast<gpu::vulkan::VulkanGraphicsSystem*>(
          emulator_->graphics_system()));
  graphics_window_->setVulkanInstance(vulkan_instance_.get());

  // Now set the graphics window as our central widget.
  QWidget* wrapper = QWidget::createWindowContainer(graphics_window_.get());
  setCentralWidget(wrapper);

  return true;
}

bool EmulatorWindow::Launch(const std::string& path) {
  return emulator_->LaunchPath(path) == X_STATUS_SUCCESS;
}

}  // namespace app
}  // namespace xe
