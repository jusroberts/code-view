#pragma once
#include "rust/cxx.h"

namespace cv {
int run_app();
void deliver_message(rust::Str json);
void deliver_connection_state(bool connected, rust::Str detail);
}
