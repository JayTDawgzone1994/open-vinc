#include "catalog.hpp"

int main() {
    const auto text = vinc::error_to_string(vinc::make_error(fixture::Code::Failure));
#ifdef EXPECT_UPDATED_CATALOG
    return text.starts_with("Updated Fixture") ? 0 : 1;
#else
    return text.find("Fixture") == std::string::npos ? 1 : 0;
#endif
}
