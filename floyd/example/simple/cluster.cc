#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <uv.h>
#include <hiredis/async.h>
#include <hiredis/adapters/libuv.h>
#include <iostream>
#include <string>
#include <algorithm>
#include "floyd/include/floyd.h"
#include "slash/include/testutil.h"

using namespace floyd;
using namespace std;

char *redisIp = "127.0.0.1";
int redisPort = 9988;
char *redisPwd = "SDxht88521378";
#define CHANNEL "ClusterChannel"
#define KEY "ClusterMembers"

Floyd *f = NULL;
char *thisIp = "127.0.0.1";
char *thisPort = NULL;
char *thisCacheDir = NULL;

string join(vector<string> &vec, const string& delim) {
    string res;
    if (vec.size() == 0) {
        return res;
    }
    for (int i=0; i<vec.size()-1; i++) {
        res += vec[i];
        res += delim;
    }
    res += vec[vec.size()-1];
    return res;
}

vector<string> split(const string& str, const string& delim) {
    vector<string> res;
    if("" == str) return res;
    //先将要切割的字符串从string类型转换为char*类型
    char * strs = new char[str.length() + 1] ; //不要忘了
    strcpy(strs, str.c_str()); 
 
    char * d = new char[delim.length() + 1];
    strcpy(d, delim.c_str());
 
    char *p = strtok(strs, d);
    while(p) {
        string s = p; //分割得到的字符串转换为string类型
        res.push_back(s); //存入结果数组
        p = strtok(NULL, d);
    }
    return res;
}

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

redisReply *runRedisCommand(char *cmd) {
    printf("Run redis command:%s\n", cmd);
    struct timeval timeout = {1, 500000}; // 1.5 seconds
    redisContext *c = redisConnectWithTimeout(redisIp, redisPort, timeout);
    redisCommand(c, "AUTH %s", redisPwd);
    redisReply *reply = (redisReply *)redisCommand(c, cmd);
    redisFree(c);
    return reply;
}

bool setRedisKV(char *key, const char *value) {
    char cmd[1024] = {0};
    sprintf(cmd, "SET %s %s", key, value);
    redisReply *reply = runRedisCommand(cmd);
    freeReplyObject(reply);
    return true;
}

string getRedisKV(char *key) {
    char cmd[1024] = {0};
    sprintf(cmd, "GET %s", key);
    redisReply *reply = runRedisCommand(cmd);
    string res;
    if (reply->str != NULL)
        res = string(reply->str);
    freeReplyObject(reply);
    return res;
}

bool pubRedis(char *channel, const char *value) {
    char cmd[1024] = {0};
    sprintf(cmd, "PUBLISH %s %s", channel, value);
    redisReply *reply = runRedisCommand(cmd);
    freeReplyObject(reply);
    return true;
}

bool push_members_to_redis(Floyd* f) {
    std::set<std::string> nodes;
    Status s = f->GetAllServers(&nodes);
    if (!s.ok()) {
        return false;
    }
    string members;
    for (const std::string& n : nodes) {
        members += n;
        members += string(",");
    }
    printf("SET %s %s\n", KEY, members.c_str());

    setRedisKV(KEY, members.c_str());
    return true;
}

void startFloydThread(void *arg) {
    //Options op("127.0.0.1:4311,127.0.0.1:4312,127.0.0.1:4313,127.0.0.1:4314", thisIp, atoi(this_port), this_cachedir);
    vector<string> current_cluster_members = split(getRedisKV(KEY), ",");
    string thisNode = string(thisIp) + string(":") + string(thisPort);
    if (find(current_cluster_members.begin(), current_cluster_members.end(), thisNode) == current_cluster_members.end()) {
        current_cluster_members.push_back(thisNode);
        char value[1024] = {0};
        sprintf(value, "AddServer#%s", thisNode.c_str());
        pubRedis(CHANNEL, value);
    }
    string current_cluster_members_str = join(current_cluster_members, ",");
    printf("current_cluster_members:%s\n", current_cluster_members_str.c_str());
    Options op(current_cluster_members_str.c_str(), thisIp, atoi(thisPort), thisCacheDir);
    op.Dump();

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
                push_members_to_redis(f);
            } else {
                s = f->Read("time", &val);
                printf("status %s, val %s\n", s.ToString().c_str(), val.c_str());
            }
        } else {
            printf("electing leader... sleep 2s\n");
        }
        sleep(2);
    }
}

void subCallback(redisAsyncContext *redisContext, void *reply, void *privdata) {
    redisReply *rReply = (redisReply *) reply;
    if (rReply == NULL || rReply->type != REDIS_REPLY_ARRAY) {
        printf("Subscribe callback error:%s\n", redisContext->errstr);
        redisAsyncDisconnect(redisContext);
        return;
    }
    //正常的订阅信息，elements是个三维数组，elements[2]是订阅信息
    if (rReply->elements != 3 || rReply->element[2]->str == NULL || rReply->element[2]->len == 0) {
        printf("订阅信息为空错误(注：订阅初始会收到一条空的信息)\n");
        return;
    }
    printf("订阅:%s\n", rReply->element[2]->str);
    if (f == NULL) {
        printf("floyd not init yet!!!\n");
        return;
    }

    slash::Status s;
    string msg(rReply->element[2]->str);
    if (msg.find("AddServer") != -1) {
        string newNode = split(msg, "#")[1];
        s = f->AddServer(newNode);
        printf("Add new server status %s\n", s.ToString().c_str());
    } else if (msg.find("DelServer") != -1) {
        string delNode = split(msg, "#")[1];
        s = f->RemoveServer(delNode);
        printf("Remove out server status %s\n", s.ToString().c_str());
    }
}

void authCallback(redisAsyncContext *redisContext, void *reply, void *privdata) {
    redisReply *rReply = (redisReply *) reply;
    printf("Auth callback:%d,%s\n", rReply->type, rReply->str);
    if (rReply == NULL || rReply->type != REDIS_REPLY_STATUS) {
        printf("Auth callback error:%s, disconnecting...\n", redisContext->errstr);
        redisAsyncDisconnect(redisContext);
        return;
    }
    if (redisAsyncCommand(redisContext, subCallback, NULL, "subscribe %s", CHANNEL) != REDIS_OK) {
        printf("Send subscribe command error: %s\n", redisContext->errstr);
        return;
    }
}

void connectCallback(const redisAsyncContext *redisContext, int status) {
    if (status != REDIS_OK) {
        printf("Connect callback error: %s\n", redisContext->errstr);
        return;
    }

    if (redisAsyncCommand((redisAsyncContext *) redisContext, authCallback, NULL, "auth %s", redisPwd) != REDIS_OK) {
        printf("Send auth command error: %s\n", redisContext->errstr);
        return;
    }
    printf("Connected...\n");
}

void disconnectCallback(const redisAsyncContext *redisContext, int status) {
    if (status != REDIS_OK) {
        printf("Disconnect error: %s\n", redisContext->errstr);
    }

    printf("Disconnected...\n");
}

void startConnectRedis() {
    uv_loop_t *loop = (uv_loop_t *)malloc(sizeof(uv_loop_t));
    uv_loop_init(loop);
    if (loop == NULL) {
        return;
    }
    redisAsyncContext *c = redisAsyncConnect(redisIp, redisPort);
    if (c != NULL && c->err) {
        printf("Error: %s\n", c->errstr);
        return;
    }
    if (redisLibuvAttach(c, loop) != REDIS_OK) {
        printf("RedisLibuvAttach error\n");
        redisAsyncDisconnect(c);
        return;
    }
    if (redisAsyncSetConnectCallback(c, connectCallback) != REDIS_OK) {
        printf("Error set connect callback\n");
        redisAsyncDisconnect(c);
        return;
    }
    if (redisAsyncSetDisconnectCallback(c, disconnectCallback) != REDIS_OK) {
        printf("Error set disconnect callback\n");
        redisAsyncDisconnect(c);
        return;
    }
    uv_run(loop, UV_RUN_DEFAULT);   // 主线程卡在这里循环
    uv_loop_close(loop);
    free(loop);
    printf("Free&exit\n");
}

int main(int argc, char **argv) {
    //thisIp = argv[1];
    thisPort = argv[1];
    thisCacheDir = argv[1];
    uv_thread_t floydThread;
    uv_thread_create(&floydThread, startFloydThread, NULL);
    startConnectRedis();
}
