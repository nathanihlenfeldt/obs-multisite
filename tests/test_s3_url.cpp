#include "s3_transport.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace multisite;
int main(){
    struct Case { const char* label; S3Config cfg; const char* expect; };
    auto mk = [](std::string acct, std::string ep, std::string bucket){
        S3Config c; c.r2_account_id=acct; c.endpoint_host=ep; c.bucket=bucket;
        c.access_key_id="k"; c.secret_access_key="s"; return c;
    };
    std::vector<Case> cases = {
      {"bare account id",        mk("abc123","","mybucket"),
       "https://abc123.r2.cloudflarestorage.com/mybucket"},
      {"account id with scheme", mk("https://abc123.r2.cloudflarestorage.com","","mybucket"),
       "https://abc123.r2.cloudflarestorage.com/mybucket"},
      {"endpoint with scheme",   mk("","https://s3.us-east-1.amazonaws.com","mybucket"),
       "https://s3.us-east-1.amazonaws.com/mybucket"},
      {"endpoint trailing slash",mk("","s3.example.com/","mybucket"),
       "https://s3.example.com/mybucket"},
      {"whitespace everywhere",  mk("  abc123 ","","  mybucket  "),
       "https://abc123.r2.cloudflarestorage.com/mybucket"},
      {"bucket with slashes",    mk("abc123","","/mybucket/"),
       "https://abc123.r2.cloudflarestorage.com/mybucket"},
      {"endpoint with path",     mk("","minio.local:9000/console","mybucket"),
       "https://minio.local:9000/mybucket"},
    };
    int bad=0;
    for (auto& c : cases) {
        S3Transport t(c.cfg);
        std::string got = t.base_url();
        bool ok = (got == c.expect);
        if (!ok) ++bad;
        std::printf("  [%s] %-26s -> %s\n", ok?"ok ":"BAD", c.label, got.c_str());
        if (!ok) std::printf("        expected %s\n", c.expect);
    }
    // and the genuinely-missing case must be reported, not malformed
    S3Config none; none.bucket="b";
    S3Transport t2(none);
    std::printf("  [%s] %-26s -> %s\n",
        t2.base_url().find("no endpoint")!=std::string::npos?"ok ":"BAD",
        "nothing configured", t2.base_url().c_str());
    std::printf("%s\n", bad? "NORMALISATION FAILED":"all paste shapes normalise correctly");
    return bad?1:0;
}
