// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include "emptyImpl.hpp"

namespace module {
namespace main {

// The module provides no interface. What it does -- measure -- is declared in the telemetry block
// of the manifest, and a manifest has to provide something.
void emptyImpl::init() {
}

void emptyImpl::ready() {
}

} // namespace main
} // namespace module
