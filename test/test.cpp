#define BOOST_TEST_MODULE yueshi_test
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_CASE(test_aaa) {
    BOOST_TEST(std::isalpha('a'));
}