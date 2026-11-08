#pragma once
#include "rust/cxx.h"

namespace cv {
int run_app();
void deliver_message(rust::Str conn, rust::Str json);
void deliver_connection_state(rust::Str conn, bool connected, rust::Str detail);
}
