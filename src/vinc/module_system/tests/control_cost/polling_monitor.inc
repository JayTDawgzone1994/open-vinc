    // Historical pre-stage-three policy, only in the isolated cost executable.
    // Current tick takes two scheduling arguments: always allow deferred retry
    // and ignore runnable backlog, matching the former once-per-wait scan.
    void monitor(std::stop_token token) {
        std::array<std::shared_ptr<Record>, module_record_limit> current;
        while (!token.stop_requested()) {
            test::control_wakeup::pass_started(true, Clock::now() + supervisor_poll_interval);
            bool ignored_runnable = false;
            std::size_t count = 0;
            {
                std::lock_guard registry_lock(mutex);
                count = records.size();
                std::copy(records.begin(), records.end(), current.begin());
            }
            for (auto& record : std::span(current).first(count)) {
                try {
                    {
                        std::lock_guard record_lock(record->mutex);
                        tick(*record, true, ignored_runnable);
                    }
                    register_name(record);
                    process_module_query(record);
                } catch (const std::system_error& error) {
                    std::lock_guard record_lock(record->mutex);
                    record_failure(*record, Error{ErrorCode::SupervisorFailed, ErrorOperation::Supervise,
                                                  osal::native_error(error.code())});
                } catch (const std::bad_alloc&) {
                    std::lock_guard record_lock(record->mutex);
                    record_failure(*record, failure(ErrorCode::ResourceExhausted, ErrorOperation::Supervise));
                } catch (...) {
                    std::lock_guard record_lock(record->mutex);
                    record_failure(*record, failure(ErrorCode::SupervisorFailed, ErrorOperation::Supervise));
                }
                record.reset();
            }
            test::control_wakeup::before_wait();
            std::unique_lock registry_lock(mutex);
            const auto status = wake.wait_for(registry_lock, supervisor_poll_interval);
            test::control_wakeup::after_wait(status);
        }
    }
