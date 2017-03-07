#ifndef ANDROID_DVR_VIRTUAL_TOUCHPAD_CLIENT_H
#define ANDROID_DVR_VIRTUAL_TOUCHPAD_CLIENT_H

#include "VirtualTouchpad.h"

namespace android {
namespace dvr {

// VirtualTouchpadClient implements a VirtualTouchpad by connecting to
// a VirtualTouchpadService over Binder.
//
class VirtualTouchpadClient : public VirtualTouchpad {
 public:
  // VirtualTouchpad implementation:
  static sp<VirtualTouchpad> Create();
  status_t Attach() override;
  status_t Detach() override;
  status_t Touch(int touchpad, float x, float y, float pressure) override;
  status_t ButtonState(int touchpad, int buttons) override;
  void dumpInternal(String8& result) override;

 protected:
  VirtualTouchpadClient() {}
  ~VirtualTouchpadClient() override {}

 private:
  VirtualTouchpadClient(const VirtualTouchpadClient&) = delete;
  void operator=(const VirtualTouchpadClient&) = delete;
};

}  // namespace dvr
}  // namespace android

#endif  // ANDROID_DVR_VIRTUAL_TOUCHPAD_CLIENT_H
