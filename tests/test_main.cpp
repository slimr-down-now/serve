#define SLIMR_TEST_MAIN
#include "serve.hpp"
#ifdef SLIMR_SERVE_HAS_HTTP
#include "serve_http.hpp"
#endif
#include "test.hpp"

#include "test_auth.hpp"
#include "test_cli.hpp"
#include "test_core.hpp"
#ifdef SLIMR_SERVE_HAS_HTTP
#include "test_daemon.hpp"
#include "test_http.hpp"
#endif
