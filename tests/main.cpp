#include "kv.h"
#include "raft_interfaces.h"

#include <easyraft.h>
#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <iomanip>
#include <cstring>
#include "chan.h"

kv g_kv;
proposeWaiter g_pw;
uint64_t self;
std::atomic<uint64_t> g_done;
chan<int> g_joinC;

const char* json[] = {
	R"({"id":1,"cluster_id":1,"snap_count":10000,"waldir":"wal1","snapdir":"snap1","tickms":100,"election_tick":10,"heartbeat_tick":1,"boostrap_timeout":1,"peers":[{"id":1,"url":"http://127.0.0.1:9001"},{"id":2,"url":"http://127.0.0.1:9002"},{"id":3,"url":"http://127.0.0.1:9003"}],"join":false,"max_size_per_msg":1048576,"max_inflight_msgs":256,"snapshot_entries":1000,"max_snap_files":5,"max_wal_files":5})",
	R"({"id":2,"cluster_id":1,"snap_count":10000,"waldir":"wal2","snapdir":"snap2","tickms":100,"election_tick":10,"heartbeat_tick":1,"boostrap_timeout":1,"peers":[{"id":1,"url":"http://127.0.0.1:9001"},{"id":2,"url":"http://127.0.0.1:9002"},{"id":3,"url":"http://127.0.0.1:9003"}],"join":false,"max_size_per_msg":1048576,"max_inflight_msgs":256,"snapshot_entries":1000,"max_snap_files":5,"max_wal_files":5})",
	R"({"id":3,"cluster_id":1,"snap_count":10000,"waldir":"wal3","snapdir":"snap3","tickms":100,"election_tick":10,"heartbeat_tick":1,"boostrap_timeout":1,"peers":[{"id":1,"url":"http://127.0.0.1:9001"},{"id":2,"url":"http://127.0.0.1:9002"},{"id":3,"url":"http://127.0.0.1:9003"}],"join":false,"max_size_per_msg":1048576,"max_inflight_msgs":256,"snapshot_entries":1000,"max_snap_files":5,"max_wal_files":5})",
};

const char* json2[] = {
	R"({"id":1,"cluster_id":2,"snap_count":10000,"waldir":"wal11","snapdir":"snap11","tickms":100,"election_tick":10,"heartbeat_tick":1,"boostrap_timeout":1,"peers":[{"id":1,"url":"http://127.0.0.1:9001"},{"id":2,"url":"http://127.0.0.1:9002"},{"id":3,"url":"http://127.0.0.1:9003"}],"join":false,"max_size_per_msg":1048576,"max_inflight_msgs":256,"snapshot_entries":1000,"max_snap_files":5,"max_wal_files":5})",
	R"({"id":2,"cluster_id":2,"snap_count":10000,"waldir":"wal12","snapdir":"snap12","tickms":100,"election_tick":10,"heartbeat_tick":1,"boostrap_timeout":1,"peers":[{"id":1,"url":"http://127.0.0.1:9001"},{"id":2,"url":"http://127.0.0.1:9002"},{"id":3,"url":"http://127.0.0.1:9003"}],"join":false,"max_size_per_msg":1048576,"max_inflight_msgs":256,"snapshot_entries":1000,"max_snap_files":5,"max_wal_files":5})",
	R"({"id":3,"cluster_id":2,"snap_count":10000,"waldir":"wal13","snapdir":"snap13","tickms":100,"election_tick":10,"heartbeat_tick":1,"boostrap_timeout":1,"peers":[{"id":1,"url":"http://127.0.0.1:9001"},{"id":2,"url":"http://127.0.0.1:9002"},{"id":3,"url":"http://127.0.0.1:9003"}],"join":false,"max_size_per_msg":1048576,"max_inflight_msgs":256,"snapshot_entries":1000,"max_snap_files":5,"max_wal_files":5})",
};

const char* joinConfig[] = {
	R"({"id":4,"cluster_id":1,"snap_count":10000,"waldir":"wal4","snapdir":"snap4","tickms":100,"election_tick":10,"heartbeat_tick":1,"boostrap_timeout":1,"peers":[{"id":1,"url":"http://127.0.0.1:9001"},{"id":2,"url":"http://127.0.0.1:9002"},{"id":3,"url":"http://127.0.0.1:9003"},{"id":4,"url":"http://127.0.0.1:9004"}],"join":true,"max_size_per_msg":1048576,"max_inflight_msgs":256,"snapshot_entries":1000,"max_snap_files":5,"max_wal_files":5})",
	R"({"id":5,"cluster_id":1,"snap_count":10000,"waldir":"wal5","snapdir":"snap5","tickms":100,"election_tick":10,"heartbeat_tick":1,"boostrap_timeout":1,"peers":[{"id":1,"url":"http://127.0.0.1:9001"},{"id":2,"url":"http://127.0.0.1:9002"},{"id":3,"url":"http://127.0.0.1:9003"},{"id":5,"url":"http://127.0.0.1:9005"}],"join":true,"max_size_per_msg":1048576,"max_inflight_msgs":256,"snapshot_entries":1000,"max_snap_files":5,"max_wal_files":5})",
};

int main(int argc, const char*argv[]) {
	if (argc != 2) {
		return -1;
	}
	char ver[256];
	RAFT_Callback cb;
	cb.free = freeSnapshot;
	cb.getSnapshot = getSnapshot;
	cb.onCommit = onCommit;
	cb.onStateChange = onStateChange;
	cb.recoverFromSnapshot = recoverFromSnapshot;
	cb.onMessage = onMessage;

	RAFT_GetVersion(ver, 256);
	std::cout << ver << std::endl;

	RAFT_SetCallback(&cb);
	RAFT_SetLogger("stdout", 0);
	RAFT_SetLogLevel(RAFT_LOG_DEBUG);

	self = atoi(argv[1]) - 1;

	uint64_t svr = 0, svr2 = 0;

	if (self >= 3) {
		svr = RAFT_NewRaftServer(0, joinConfig[self - 3]);
	} else {
		g_joinC.close();
		svr = RAFT_NewRaftServer((void*)1, json[self]);
		svr2 = RAFT_NewRaftServer((void*)2, json2[self]);
	}

	std::cout << svr << std::endl;
	std::string pro;
	std::vector<std::string> arg;
	std::string cmd;
	std::string v;
	uint64_t cid = 0;

	auto propose_wait = [&](bool wait = true) ->bool
	{
		auto ret = false;
		auto size = propose::header_length() + pro.length();
		auto mem = new char[size];
		auto& pr = *reinterpret_cast<propose*>(mem);
		pr.id = self;
		pr.cid = cid;
		pr.seq = g_pw.getSeq();
		memcpy(pr.cmd, pro.c_str(), pro.length());
		if (RAFT_Propose(svr, mem, (int)size, 3000) != 0) {
			std::cout << cmd << " failed." << std::endl;
		}
		else {
			if (wait) {
				g_pw.wait(pr.seq);
			}
			ret = true;
		}
		delete[]mem;
		return ret;
	};

	uint64_t svrs[3] = {0, svr, svr2};

	while(true) {
		if (RAFT_GetPeersStatus(svrs[1], ver, 256) == 0) {
			printf("1: peers status %s\n", ver);
		}
		if (RAFT_GetPeersStatus(svrs[2], ver, 256) == 0) {
			printf("2: peers status %s\n", ver);
		}
		char buf[256] = {0};
		RAFT_SendMessage(svrs[1], 2, "123", 3, buf, 256); 
		printf("sendmessage return: %s\n", buf);
		std::getline(std::cin, pro);
		pro = pro.substr(0, pro.find_last_of('\n'));
		splitCmd(pro, cmd, cid, arg);
		if (cmd == "exit") {
			break;
		}
		svr = svrs[cid];
		if (cid > 2) {
			printf("clusterId error.");
			continue;
		}
		else if (cmd == "snapshot") {
			RAFT_Snapshot(svr);
		} 
		else if (cmd == "get" && arg.size() >= 1u) {
			if (g_kv.get(arg[0], v)) {
				std::cout << "value: " << v << std::endl;
			} else {
				std::cout << "key not found" << std::endl;
			}
		}
		else if(cmd == "sget" && arg.size() >= 1u) {
			if (propose_wait()) {
				if (g_kv.get(arg[0], v)) {
					std::cout << "value: " << v << std::endl;
				}
				else {
					std::cout << "key not found" << std::endl;
				}
			}
		}
		else if (cmd == "set" && arg.size() >= 2u) {
			if (propose_wait()) {
				std::cout << "set done" << std::endl;
			}
		}
		else if (cmd == "app" && arg.size() >= 2u) {
			if (propose_wait()) {
				std::cout << "app done" << std::endl;
			}
		}
		else if (cmd == "del" && arg.size() >= 1u) {
			if (propose_wait()) {
				std::cout << "del done" << std::endl;
			}
		}
		else if (cmd == "adds" && arg.size() >= 2u) {
			RAFT_AddServer(svr, std::atoi(arg[0].c_str()), arg[1].c_str());
		}
		else if (cmd == "dels" && arg.size() >= 1u) {
			RAFT_DelServer(svr, std::atoi(arg[0].c_str()));
		}
		else if (cmd == "gogogo") {
			g_joinC.close();
		}
		else if (cmd == "bench") {
			RAFT_SetLogger(0, 0);
			auto benchSize = 100000;
			auto propSize = 512;
			if (arg.size() >= 1u) {
				benchSize = std::atoi(arg[0].c_str());
			}
			auto step = benchSize / 10;
			auto cur = step;
			pro = "set hello ";
			for(auto i=0; i<propSize; i++) {
				pro += "a";
			}
			auto start = std::chrono::high_resolution_clock::now();
			for(auto i=0; i<benchSize - 1; i++) {
				propose_wait(false);
				if (i >= cur) {
					std::cout << i << " ops done." << std::endl;
					cur += step;
				}
			}
			auto cp1 = std::chrono::high_resolution_clock::now();
			pro = "set hello ";
			for (auto i = 0; i<propSize; i++) {
				pro += "b";
			}
			propose_wait(true);
			std::cout << benchSize << " ops done." << std::endl;
			auto cp2 = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double, std::milli> dms = cp1 - start;
			auto ds = std::chrono::duration_cast<std::chrono::duration<double>>(dms);
			std::cout.setf(std::ios::fixed);
			std::cout << "send propose " << benchSize << " times cost " << std::setprecision(2) << dms.count() << " ms. " << " qps: " << std::setprecision(2) << benchSize / ds.count() << std::endl;
			dms = cp2 - start;
			ds = std::chrono::duration_cast<std::chrono::duration<double>>(dms);
			std::cout << "done propose " << benchSize << " times cost " << std::setprecision(2) << dms.count() << " ms. " << " qps: " << std::setprecision(2) << benchSize / ds.count() << std::endl;
			RAFT_SetLogger("stdout", 0);
		}
		else if (cmd == "help") {
			std::cout << "exit           : end server"                << std::endl;
			std::cout << "snapshot <cid> : force generate a snapshot" << std::endl;
			std::cout << "get k    <cid> : dirty get k v"             << std::endl;
			std::cout << "sget k   <cid> : safe get k v"              << std::endl;
			std::cout << "app k v  <cid> : append k v"                << std::endl;
			std::cout << "del k    <cid> : delete key k"              << std::endl;
		}
		else {
			std::cout << "illegal command, type help to get command list." << std::endl;
			//RAFT_Propose(svr, (void*)pro.c_str(), (int)pro.length());
		}
	}
	// must stop pending recovery
	g_joinC.close();
	RAFT_DeleteRaftServer(svrs[1]);
	RAFT_DeleteRaftServer(svrs[2]);
	return 0;
}

void splitCmd(const std::string& line_, std::string& cmd, uint64_t& cid, std::vector<std::string>& arg) {
	auto line = line_;
	arg.clear();
	auto inColon = false;
	for (auto& c : line) {
		if (c == ' ' && !inColon) {
			c = '\x0';
		}
		else if (c == '"') {
			inColon = !inColon;
		}
	}
	auto pos = line.find_first_of('\x0');
	cmd = line.substr(0, pos);
	std::for_each(cmd.begin(), cmd.end(), [](char c) -> char { return tolower(c); });
	cid = 0;
	while (pos != line.npos) {
		auto beg = pos + 1;
		pos = line.find_first_of('\x0', beg);
		if (cid == 0) {
			cid = atoll(line.substr(beg, pos - beg).c_str());
		} else if (pos != line.npos) {
			arg.push_back(line.substr(beg, pos - beg));
		}
		else {
			arg.push_back(line.substr(beg));
		}
	}
}
