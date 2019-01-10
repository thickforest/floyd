#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>

#include <iostream>
#include <string>

#include "floyd/include/floyd.h"
#include "slash/include/testutil.h"

using namespace floyd;
uint64_t NowMicros() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
}

bool print_members(Floyd* f) {
  std::set<std::string> nodes;
  Status s = f->GetAllServers(&nodes);
  if (!s.ok()) {
    return false;
  }
  printf("Membership: ");
  for (const std::string& n : nodes) {
    printf(" %s", n.c_str());
  }
  printf("\n");
  return true;
}

int main(int argc, char **argv)
{
  Options op("127.0.0.1:4311,127.0.0.1:4312,127.0.0.1:4313,127.0.0.1:4314", "127.0.0.1", atoi(argv[1]), argv[2]);
  op.Dump();

  Floyd *f;

  slash::Status s;
  s = Floyd::Open(op, &f);
  printf("start floyd f status %s\n", s.ToString().c_str());

  std::string msg;
  std::string val;

  while (1) {
    if (f->HasLeader()) {
      print_members(f);
      //f->GetServerStatus(&msg);
      //printf("%s\n", msg.c_str());
      if (f->IsLeader()) {
        val = std::to_string(NowMicros());
        f->Write("time", val);
        printf("I'm leader, val %s\n", val.c_str());
      } else {
        s = f->Read("time", &val);
        printf("status %s, val %s\n", s.ToString().c_str(), val.c_str());
      }
    } else {
      printf("electing leader... sleep 2s\n");
    }
    sleep(2);
  }

  return 0;
}
