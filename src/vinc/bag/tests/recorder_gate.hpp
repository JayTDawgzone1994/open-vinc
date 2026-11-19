#pragma once

namespace vinc::bag::test {
// Test-only rendezvous after dequeue and before canonicalization. The production
// library has no hook, branch or callback at this boundary.
void before_canonical();
}
