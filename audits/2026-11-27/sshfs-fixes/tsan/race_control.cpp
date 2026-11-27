#include <atomic>
#include <thread>
int shared_value = 0;
int main() {
    std::atomic<bool> go{false};
    auto write = [&] {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < 10000; ++i) shared_value += 1; // intentional race, harness control
    };
    std::thread first(write), second(write);
    go.store(true, std::memory_order_release);
    first.join(); second.join();
    return shared_value == -1;
}
