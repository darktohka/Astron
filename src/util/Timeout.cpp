#include "Timeout.h"
#include "core/global.h"

Timeout::Timeout(unsigned long ms, std::function<void()> f) :
    m_loop(g_loop),
    m_timer(nullptr),
    m_callback_disabled(false)
{
    initialize(ms, f);
}

Timeout::Timeout() :
    m_loop(g_loop),
    m_timer(nullptr),
    m_callback_disabled(false)
{
}

void Timeout::initialize(unsigned long ms, TimeoutCallback callback)
{
    assert(std::this_thread::get_id() == g_main_thread_id);

    m_timeout_interval = ms;
    m_callback = callback;
}

void Timeout::setup()
{
    assert(m_timer == nullptr);

    m_timer = m_loop->resource<uvw::timer_handle>();

    m_timer->on<uvw::timer_event>([self = this](const uvw::timer_event&, uvw::timer_handle&) {
        self->timer_callback();
    });
}

void Timeout::destroy_timer()
{
    m_callback = nullptr;

    if(m_timer) {
        m_timer->stop();
        m_timer->close();
        m_timer = nullptr;
    }

    m_lifetime->invalidate();
    delete this;
}

void Timeout::timer_callback()
{
    assert(std::this_thread::get_id() == g_main_thread_id);

    if(m_callback != nullptr && !m_callback_disabled.exchange(true)) {
        m_callback();
    }

    destroy_timer();
}

void Timeout::reset()
{
    assert(std::this_thread::get_id() == g_main_thread_id);

    if(m_timer == nullptr) {
        setup();
    }

    m_timer->stop();
    m_timer->start(uvw::timer_handle::time{m_timeout_interval}, uvw::timer_handle::time{0});
}

bool Timeout::cancel()
{
    const bool already_cancelled = !m_callback_disabled.exchange(true);

    if(std::this_thread::get_id() != g_main_thread_id) {
        // Defer the cancellation to the main thread.
        // Guard the task at the same time.
        LifetimeGuardPtr lifetime = m_lifetime;
        TaskQueue::singleton.enqueue_task([this, lifetime]() {
            std::lock_guard<std::recursive_mutex> lock(lifetime->mutex);
            if(!lifetime->alive.load()) {
                // We have already been destroyed. Nothing to cancel.
                return;
            }
            cancel();
        });

        return already_cancelled;
    }

    if(m_timer != nullptr) {
        destroy_timer();
    }

    return already_cancelled;
}

Timeout::~Timeout()
{
    m_lifetime->invalidate();
    assert(m_callback_disabled.load());
}
