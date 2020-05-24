#pragma once

#include "model/timeout_clock.h"
#include "storage/compacted_index.h"

#include <seastar/core/circular_buffer.hh>
#include <seastar/core/file.hh>

#include <memory>
namespace storage {

// clang-format off
CONCEPT(template<typename Consumer> concept bool CompactedIndexEntryConsumer() {
    return requires(Consumer c, compacted_index::entry&& b) {
        { c(std::move(b)) } -> ss::future<ss::stop_iteration>;
        c.end_of_stream();
    };
})
// clang-format on

class compacted_index_reader {
public:
    class impl {
    public:
        explicit impl(ss::sstring filename) noexcept
          : _name(std::move(filename)) {}
        virtual ~impl() noexcept = default;
        impl(impl&&) noexcept = default;
        impl& operator=(impl&&) noexcept = default;
        impl(const impl&) = delete;
        impl& operator=(const impl&) = delete;

        virtual ss::future<> close() = 0;

        virtual ss::future<compacted_index::footer> load_footer() = 0;

        virtual void print(std::ostream&) const = 0;

        const ss::sstring& filename() const { return _name; }

        virtual bool is_end_of_stream() const = 0;

        virtual ss::future<ss::circular_buffer<compacted_index::entry>>
          load_slice(model::timeout_clock::time_point) = 0;

        template<typename Consumer>
        auto
        consume(Consumer consumer, model::timeout_clock::time_point timeout) {
            return ss::do_with(
              std::move(consumer), [this, timeout](Consumer& consumer) {
                  return do_consume(consumer, timeout);
              });
        }

    private:
        compacted_index::entry pop_batch() {
            compacted_index::entry batch = std::move(_slice.front());
            _slice.pop_front();
            return batch;
        }

        bool is_slice_empty() const { return _slice.empty(); }

        ss::future<> do_load_slice(model::timeout_clock::time_point t) {
            return load_slice(t).then(
              [this](ss::circular_buffer<compacted_index::entry> next) {
                  _slice = std::move(next);
              });
        }

        template<typename Consumer>
        auto do_consume(
          Consumer& consumer, model::timeout_clock::time_point timeout) {
            return ss::repeat([this, timeout, &consumer] {
                       if (likely(!is_slice_empty())) {
                           return consumer(pop_batch());
                       }
                       if (is_end_of_stream()) {
                           return ss::make_ready_future<ss::stop_iteration>(
                             ss::stop_iteration::yes);
                       }
                       return do_load_slice(timeout).then(
                         [] { return ss::stop_iteration::no; });
                   })
              .then([&consumer] { return consumer.end_of_stream(); });
        }

        ss::sstring _name;
        ss::circular_buffer<compacted_index::entry> _slice;
    };

    explicit compacted_index_reader(std::unique_ptr<impl> i) noexcept
      : _impl(std::move(i)) {}

    ss::future<> close();

    ss::future<compacted_index::footer> load_footer() {
        return _impl->load_footer();
    }

    void print(std::ostream& o) const { _impl->print(o); }

    const ss::sstring& filename() const { return _impl->filename(); }

    template<typename Consumer>
    CONCEPT(requires CompactedIndexEntryConsumer<Consumer>())
    auto consume(
      Consumer consumer, model::timeout_clock::time_point timeout) & {
        return _impl->consume(std::move(consumer), timeout);
    }

    /// \brief same as above, except used with r-values:
    /// return std::move(reader).consume(functor{}, model::no_timeout);
    ///
    template<typename Consumer>
    CONCEPT(requires CompactedIndexEntryConsumer<Consumer>())
    auto consume(
      Consumer consumer, model::timeout_clock::time_point timeout) && {
        auto raw = _impl.get();
        return raw->consume(std::move(consumer), timeout)
          .finally([i = std::move(_impl)] {});
    }

private:
    std::unique_ptr<impl> _impl;
};

} // namespace storage