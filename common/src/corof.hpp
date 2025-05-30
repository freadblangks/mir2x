#pragma once
#include <optional>
#include <coroutine>
#include <exception>
#include "fflerror.hpp"
#include "raiitimer.hpp"

namespace corof
{
    namespace _details
    {
        struct base_promise
        {
            base_promise *inner_promise = nullptr;
            base_promise *outer_promise = nullptr;

            virtual ~base_promise() = default;
            virtual std::coroutine_handle<> get_handle() = 0;
        };

        struct eval_poller_base_promise: public base_promise
        {
            std::exception_ptr exceptr = nullptr;
            auto initial_suspend() noexcept
            {
                return std::suspend_always{};
            }

            auto final_suspend() noexcept
            {
                return std::suspend_always{};
            }

            void unhandled_exception()
            {
                exceptr = std::current_exception();
            }

            void rethrow_if_unhandled_exception()
            {
                if(exceptr){
                    std::rethrow_exception(exceptr);
                }
            }
        };

        struct eval_poller_promise_with_void: public _details::eval_poller_base_promise
        {
            void return_void() {}
        };

        template<typename T> struct eval_poller_promise_with_type: public _details::eval_poller_base_promise
        {
            T value;
            void return_value(T t)
            {
                value = std::move(t);
            }
        };
    }

    template<typename T = void> class [[nodiscard]] eval_poller
    {
        public:
            struct promise_type: public std::conditional_t<std::is_void_v<T>, _details::eval_poller_promise_with_void, _details::eval_poller_promise_with_type<T>>
            {
                std::coroutine_handle<> get_handle() override
                {
                    return std::coroutine_handle<promise_type>::from_promise(*this);
                }

                eval_poller get_return_object()
                {
                    return eval_poller{std::coroutine_handle<promise_type>::from_promise(*this)};
                }
            };

        private:
            class [[nodiscard]] awaiter
            {
                private:
                    friend class eval_poller;

                private:
                    std::coroutine_handle<eval_poller::promise_type> m_handle;

                private:
                    explicit awaiter(std::coroutine_handle<eval_poller::promise_type> handle)
                        : m_handle(handle)
                    {}

                public:
                    awaiter              (      awaiter &&) = delete;
                    awaiter              (const awaiter  &) = delete;
                    awaiter & operator = (      awaiter &&) = delete;
                    awaiter & operator = (const awaiter  &) = delete;

                public:
                    bool await_ready() noexcept
                    {
                        return false;
                    }

                public:
                    template<typename OuterPrimise> void await_suspend(std::coroutine_handle<OuterPrimise> h) noexcept
                    {
                        /**/   h.promise().inner_promise = std::addressof(m_handle.promise());
                        m_handle.promise().outer_promise = std::addressof(       h.promise());
                    }

                public:
                    auto await_resume()
                    {
                        if constexpr(std::is_void_v<T>){
                            return;
                        }
                        else{
                            return m_handle.promise().value;
                        }
                    }
            };

        private:
            std::coroutine_handle<eval_poller::promise_type> m_handle;

        public:
            explicit eval_poller(std::coroutine_handle<eval_poller::promise_type> handle = nullptr)
                : m_handle(handle)
            {}

        public:
            eval_poller(eval_poller && other) noexcept
            {
                std::swap(m_handle, other.m_handle);
            }

        public:
            eval_poller & operator = (eval_poller && other) noexcept
            {
                std::swap(m_handle, other.m_handle);
                return *this;
            }

        public:
            eval_poller              (const eval_poller &) = delete;
            eval_poller & operator = (const eval_poller &) = delete;

        public:
            ~eval_poller()
            {
                if(m_handle){
                    m_handle.destroy();
                }
            }

        public:
            bool valid() const
            {
                return m_handle.address();
            }

        public:
            bool poll()
            {
                fflassert(m_handle);
                auto curr_promise = find_promise(std::addressof(m_handle.promise()));

                if(curr_promise->get_handle().done()){
                    if(!curr_promise->outer_promise){
                        return true;
                    }

                    // jump out for one layer
                    // should I call destroy() for done handle?

                    auto outer_promise = curr_promise->outer_promise;

                    curr_promise = outer_promise;
                    curr_promise->inner_promise = nullptr;
                }

                // resume only once and return immediately
                // after resume curr_handle can be in done state, next call to poll should unlink it

                curr_promise->get_handle().resume();
                return m_handle.done();
            }

        private:
            static inline _details::base_promise *find_promise(_details::base_promise *promise)
            {
                auto curr_promise = promise;
                auto next_promise = promise->inner_promise;

                while(curr_promise && next_promise){
                    curr_promise = next_promise;
                    next_promise = next_promise->inner_promise;
                }
                return curr_promise;
            }

        public:
            decltype(auto) sync_eval()
            {
                while(!poll()){
                    continue;
                }
                return awaiter(m_handle).await_resume();
            }

        public:
            awaiter operator co_await()
            {
                return awaiter(m_handle);
            }
    };
}

namespace corof
{
    template<typename T> class async_variable
    {
        private:
            std::optional<T> m_var;
            corof::eval_poller<T> m_poller;

        public:
            template<typename U = T> void assign(U && u)
            {
                fflassert(!m_var.has_value());
                m_var = std::make_optional<T>(std::move(u));
            }

        public:
            async_variable()
                : m_poller(create_poller())
            {}

        public:
            template<typename U    > async_variable                 (const async_variable<U> &) = delete;
            template<typename U = T> async_variable<T> & operator = (const async_variable<U> &) = delete;

        public:
            auto operator co_await() noexcept
            {
                return m_poller.operator co_await();
            }

        private:
            corof::eval_poller<T> create_poller()
            {
                while(!m_var.has_value()){
                    co_await std::suspend_always{};
                }
                co_return m_var.value();
            }
    };

    inline corof::eval_poller<size_t> async_wait(uint64_t msec)
    {
        size_t count = 0;
        if(msec == 0){
            co_await std::suspend_always{};
            count++;
        }
        else{
            hres_timer timer;
            while(timer.diff_msec() < msec){
                co_await std::suspend_always{};
                count++;
            }
        }
        co_return count;
    }
}

namespace corof
{
    namespace _details
    {
        struct awaitable_promise_with_void
        {
            void return_void() {}
        };

        template<typename T> struct awaitable_promise_with_type
        {
            std::optional<T> m_result;
            void return_value(T t)
            {
                m_result = std::move(t);
            }
        };
    }

    template<typename T = void> class [[nodiscard]] awaitable
    {
        private:
            class AwaitableAsAwaiter;

        public:
            class promise_type: public std::conditional_t<std::is_void_v<T>, _details::awaitable_promise_with_void, _details::awaitable_promise_with_type<T>>
            {
                private:
                    friend class AwaitableAsAwaiter;

                private:
                    class AwaitablePromiseFinalAwaiter
                    {
                        private:
                            std::coroutine_handle<promise_type> m_handle = nullptr;

                        public:
                            AwaitablePromiseFinalAwaiter() = default;

                        public:
                            ~AwaitablePromiseFinalAwaiter()
                            {
                                // put coroutine handle in final awaiter
                                // instead of inside corof::awaitable, because we have code like
                                //
                                //      co_await on_AM_MSG();
                                //
                                // if put coroutine handle inside corof::awaitable
                                // it destructs after this line, however inside on_AM_MSG() we may wait some resp messges
                                //
                                // we have registered the handle to actor resp handler list
                                // we can not let the coroutine handle in corof::awaitable destroy itself when above line returns
                                // instead we can safely destroy it only when whole function body finishes
                                m_handle.destroy();
                            }

                        public:
                            constexpr bool await_ready() const noexcept
                            {
                                return false;
                            }

                            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> handle) noexcept
                            {
                                // gcc (till 15.1) has problem to support symmetric transfer
                                // if optimization level is O2 or less, it doesn't do TCO (tail call optimization)
                                // check: https://godbolt.org/z/ac57Tzorf
                                return (m_handle = handle).promise().m_continuation;
                            }

                            constexpr void await_resume() const noexcept {}
                    };

                private:
                    std::coroutine_handle<> m_continuation;

                public:
                    awaitable get_return_object() noexcept
                    {
                        return awaitable(std::coroutine_handle<promise_type>::from_promise(*this));
                    }

                    std::suspend_always        initial_suspend() const noexcept { return {}; }
                    AwaitablePromiseFinalAwaiter final_suspend() const noexcept { return {}; }

                    void unhandled_exception()
                    {
                        std::rethrow_exception(std::current_exception());
                    }
            };

        private:
            class AwaitableAsAwaiter
            {
                private:
                    std::coroutine_handle<promise_type> m_handle;

                public:
                    explicit AwaitableAsAwaiter(std::coroutine_handle<promise_type> h)
                        : m_handle(h)
                    {}

                public:
                    bool await_ready() const noexcept
                    {
                        return !m_handle;
                    }

                    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept
                    {
                        m_handle.promise().m_continuation = h;
                        return m_handle;
                    }

                    auto await_resume()
                    {
                        if constexpr(std::is_void_v<T>){
                            return;
                        }
                        else{
                            return m_handle.promise().m_result.value();
                        }
                    }
            };

        private:
            std::coroutine_handle<promise_type> m_handle;

        public:
            explicit awaitable(std::coroutine_handle<awaitable::promise_type> h)
                : m_handle(h)
            {}

        public:
            awaitable()
                : m_handle(nullptr)
            {}

        public:
            awaitable(awaitable && other) noexcept
            {
                std::swap(m_handle, other.m_handle);
            }

        public:
            awaitable & operator = (awaitable && other) noexcept
            {
                std::swap(m_handle, other.m_handle);
                return *this;
            }

        public:
            awaitable              (const awaitable &) = delete;
            awaitable & operator = (const awaitable &) = delete;

        public:
            auto operator co_await() &&
            {
                return AwaitableAsAwaiter(m_handle);
            }

        public:
            void resume()
            {
                if(m_handle){
                    AwaitableAsAwaiter(m_handle).await_suspend(std::noop_coroutine()).resume();
                }
            }

            bool empty() const
            {
                return !m_handle;
            }
    };
}
