/**
 * rollback_session.cpp — rollback netplay for VS battles, driven by GekkoNet.
 *
 * When a VS battle starts in a netplay session, every frame:
 *   1. the local controller is sampled and handed to GekkoNet;
 *   2. GekkoNet answers with Save / Load / Advance events. Saves and loads
 *      go through PortRollbackSave/Load; an Advance stages every player's
 *      input for that tick and runs it — headless while re-simulating,
 *      rendered for the newest tick.
 * Outside a session the frame loop runs one normal tick as before.
 *
 * Native transport is UDP (for local testing). Configuration (debug):
 *   SSB64_ROLLBACK=1               enable
 *   SSB64_ROLLBACK_PLAYERS=2       players in the match
 *   SSB64_ROLLBACK_LOCAL=0         this instance's player slot
 *   SSB64_ROLLBACK_PORT=7000       local UDP port
 *   SSB64_ROLLBACK_PEER=host:port  remote address (2-player sessions)
 *   SSB64_ROLLBACK_DELAY=2         local input delay in frames
 *   SSB64_ROLLBACK_WINDOW=8        max prediction (rollback) frames
 *   SSB64_ROLLBACK_SCRIPTED=1      drive the local player from the loaded
 *                                  replay file instead of the controller
 */

#include "rollback_session.h"
#include "rollback_state.h"
#include "port_log.h"

#include <gekkonet.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

extern "C" {
void PortRunHeadlessTick(void);
void PortRunRenderedTick(void);

unsigned int syNetInputGetTick(void);
void syNetInputSetSlotSource(int player, int source);
void syNetInputSetSavedInput(int player, unsigned int tick, unsigned short buttons, signed char stick_x,
                             signed char stick_y);

/* Mirrors SYNetInputFrame (decomp/src/sys/netinput.h). */
struct PortNetInputFrame {
	uint32_t tick;
	uint16_t buttons;
	int8_t stick_x;
	int8_t stick_y;
	uint8_t source;
	uint8_t is_predicted;
	uint8_t is_valid;
};
int syNetReplayGetLoadedFrame(int player, unsigned int tick, PortNetInputFrame *out);

/* Mirrors SYNetSyncTickHash (decomp/src/sys/netsync.h). */
struct PortSessionTickHash {
	uint32_t column[12];
};
void syNetSyncHashTick(PortSessionTickHash *out);

/* The decomp's 6-byte OSContPad (PR/os.h). */
struct PortContPad {
	uint16_t button;
	int8_t stick_x;
	int8_t stick_y;
	uint8_t err;
	uint8_t pad;
};
void osContGetReadData(PortContPad *pads);

extern unsigned char gSCManagerSceneData; /* first byte is scene_curr */
extern void *gPortArenaSimStart;
}

namespace {

constexpr unsigned char kSceneVSBattle = 22;
constexpr int kSourceSaved = 3; /* nSYNetInputSourceSaved */
constexpr unsigned kStateCapacity = 4 * 1024 * 1024;

/* What travels over the network per player per frame. */
struct NetInput {
	uint16_t buttons;
	int8_t stick_x;
	int8_t stick_y;
};
static_assert(sizeof(NetInput) == 4, "NetInput must be 4 bytes");

enum class State { Idle, Pending, Running };

struct Config {
	bool enabled = false;
	int players = 2;
	int local = 0;
	int port = 7000;
	std::string peer;
	int delay = 2;
	int window = 8;
	bool scripted = false;
};

Config sConfig;
State sState = State::Idle;
GekkoSession *sSession = nullptr;
bool sStarted = false;
RollbackBuffer sSaveScratch;
unsigned sFrames = 0, sRollbacks = 0, sResimTicks = 0, sStalls = 0, sDesyncs = 0;

/* Bad-network simulation settings (see the UDP transport below). */
int sSimLatency = 0, sSimJitter = 0, sSimLoss = 0;
uint32_t sSimRng = 0x12345678u;

int EnvInt(const char *name, int fallback)
{
	const char *v = std::getenv(name);
	return (v != nullptr && v[0] != '\0') ? std::atoi(v) : fallback;
}

void LoadConfig()
{
	static bool sLoaded = false;
	if (sLoaded) return;
	sLoaded = true;
	sConfig.enabled = EnvInt("SSB64_ROLLBACK", 0) != 0;
	sConfig.players = EnvInt("SSB64_ROLLBACK_PLAYERS", 2);
	sConfig.local = EnvInt("SSB64_ROLLBACK_LOCAL", 0);
	sConfig.port = EnvInt("SSB64_ROLLBACK_PORT", 7000);
	const char *peer = std::getenv("SSB64_ROLLBACK_PEER");
	sConfig.peer = peer ? peer : "";
	sConfig.delay = EnvInt("SSB64_ROLLBACK_DELAY", 2);
	sConfig.window = EnvInt("SSB64_ROLLBACK_WINDOW", 8);
	sConfig.scripted = EnvInt("SSB64_ROLLBACK_SCRIPTED", 0) != 0;
#if !defined(_WIN32)
	sSimLatency = EnvInt("SSB64_ROLLBACK_SIM_LATENCY", 0);
	sSimJitter = EnvInt("SSB64_ROLLBACK_SIM_JITTER", 0);
	sSimLoss = EnvInt("SSB64_ROLLBACK_SIM_LOSS", 0);
	sSimRng ^= (uint32_t)sConfig.port * 2654435761u;
#endif
}

/* ------------------------------------------------------------------------- */
/*  UDP transport (native testing)                                           */
/* ------------------------------------------------------------------------- */

#if !defined(_WIN32)
int sSocket = -1;
sockaddr_in sPeerAddr{};
std::vector<GekkoNetResult *> sReceived;

bool ParseAddress(const std::string &text, sockaddr_in &out)
{
	size_t colon = text.rfind(':');
	if (colon == std::string::npos) return false;
	std::memset(&out, 0, sizeof(out));
#if defined(__APPLE__)
	out.sin_len = sizeof(out);
#endif
	out.sin_family = AF_INET;
	out.sin_port = htons((uint16_t)std::atoi(text.c_str() + colon + 1));
	return inet_pton(AF_INET, text.substr(0, colon).c_str(), &out.sin_addr) == 1;
}

/* Optional bad-network simulation for testing (outgoing packets only):
 *   SSB64_ROLLBACK_SIM_LATENCY=ms  SSB64_ROLLBACK_SIM_JITTER=ms
 *   SSB64_ROLLBACK_SIM_LOSS=percent */
struct DelayedPacket {
	std::chrono::steady_clock::time_point due;
	sockaddr_in to;
	std::vector<char> data;
};
std::vector<DelayedPacket> sDelayed;

uint32_t SimRand()
{
	sSimRng ^= sSimRng << 13;
	sSimRng ^= sSimRng >> 17;
	sSimRng ^= sSimRng << 5;
	return sSimRng;
}

void FlushDelayed()
{
	auto now = std::chrono::steady_clock::now();
	for (size_t i = 0; i < sDelayed.size();) {
		if (sDelayed[i].due <= now) {
			sendto(sSocket, sDelayed[i].data.data(), sDelayed[i].data.size(), 0, (const sockaddr *)&sDelayed[i].to,
			       sizeof(sockaddr_in));
			sDelayed.erase(sDelayed.begin() + (long)i);
		} else {
			i++;
		}
	}
}

void UdpSend(GekkoNetAddress *addr, const char *data, int length)
{
	if (sSocket < 0) return;
	if (sSimLatency == 0 && sSimJitter == 0 && sSimLoss == 0) {
		sendto(sSocket, data, (size_t)length, 0, (const sockaddr *)addr->data, (socklen_t)addr->size);
		return;
	}
	if (sSimLoss > 0 && (int)(SimRand() % 100) < sSimLoss) return;
	int ms = sSimLatency + (sSimJitter > 0 ? (int)(SimRand() % (unsigned)(2 * sSimJitter + 1)) - sSimJitter : 0);
	DelayedPacket pkt;
	pkt.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms < 0 ? 0 : ms);
	std::memcpy(&pkt.to, addr->data, sizeof(sockaddr_in));
	pkt.data.assign(data, data + length);
	sDelayed.push_back(std::move(pkt));
}

/* GekkoNet matches senders by comparing address bytes, so received
 * addresses are normalized exactly like the configured peer address. */
GekkoNetResult **UdpReceive(int *length)
{
	FlushDelayed();
	sReceived.clear();
	for (;;) {
		char buf[2048];
		sockaddr_in from{};
		socklen_t from_len = sizeof(from);
		ssize_t n = recvfrom(sSocket, buf, sizeof(buf), 0, (sockaddr *)&from, &from_len);
		if (n <= 0) break;
		sockaddr_in norm{};
#if defined(__APPLE__)
		norm.sin_len = sizeof(norm);
#endif
		norm.sin_family = AF_INET;
		norm.sin_port = from.sin_port;
		norm.sin_addr = from.sin_addr;
		auto *res = (GekkoNetResult *)std::malloc(sizeof(GekkoNetResult));
		res->addr.data = std::malloc(sizeof(norm));
		std::memcpy(res->addr.data, &norm, sizeof(norm));
		res->addr.size = sizeof(norm);
		res->data = std::malloc((size_t)n);
		std::memcpy(res->data, buf, (size_t)n);
		res->data_len = (unsigned)n;
		sReceived.push_back(res);
	}
	*length = (int)sReceived.size();
	return sReceived.data();
}

void UdpFree(void *ptr)
{
	std::free(ptr);
}

GekkoNetAdapter sUdpAdapter = { UdpSend, UdpReceive, UdpFree };

bool OpenUdp()
{
	sSocket = socket(AF_INET, SOCK_DGRAM, 0);
	if (sSocket < 0) return false;
	sockaddr_in local{};
#if defined(__APPLE__)
	local.sin_len = sizeof(local);
#endif
	local.sin_family = AF_INET;
	local.sin_port = htons((uint16_t)sConfig.port);
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(sSocket, (sockaddr *)&local, sizeof(local)) != 0) {
		close(sSocket);
		sSocket = -1;
		return false;
	}
	fcntl(sSocket, F_SETFL, fcntl(sSocket, F_GETFL) | O_NONBLOCK);
	return true;
}

void CloseUdp()
{
	if (sSocket >= 0) close(sSocket);
	sSocket = -1;
}
#endif

/* ------------------------------------------------------------------------- */
/*  Session                                                                  */
/* ------------------------------------------------------------------------- */

bool InVSBattle()
{
	return gSCManagerSceneData == kSceneVSBattle && gPortArenaSimStart != nullptr;
}

uint32_t StateChecksum()
{
	PortSessionTickHash h;
	syNetSyncHashTick(&h);
	return h.column[0]; /* folded hash of every gameplay column */
}

void EndSession(const char *why)
{
	if (sSession != nullptr) {
		port_log("SSB64 Rollback: session ended (%s) frames=%u rollbacks=%u resim_ticks=%u stalls=%u desyncs=%u\n",
		         why, sFrames, sRollbacks, sResimTicks, sStalls, sDesyncs);
		gekko_destroy(&sSession);
	}
#if !defined(_WIN32)
	CloseUdp();
#endif
	sSession = nullptr;
	sStarted = false;
	sState = State::Idle;
}

bool StartSession()
{
#if defined(_WIN32)
	port_log("SSB64 Rollback: native UDP transport is POSIX-only\n");
	return false;
#else
	if (sConfig.players != 2 || !ParseAddress(sConfig.peer, sPeerAddr)) {
		port_log("SSB64 Rollback: need SSB64_ROLLBACK_PLAYERS=2 and SSB64_ROLLBACK_PEER=host:port\n");
		return false;
	}
	if (!OpenUdp()) {
		port_log("SSB64 Rollback: cannot bind UDP port %d\n", sConfig.port);
		return false;
	}
	if (!gekko_create(&sSession, GekkoGameSession)) {
		CloseUdp();
		return false;
	}
	GekkoConfig config{};
	config.num_players = (unsigned char)sConfig.players;
	config.max_spectators = 0;
	config.max_input_prediction_window = (unsigned char)sConfig.window;
	config.input_size = sizeof(NetInput);
	config.state_size = kStateCapacity;
	config.limited_saving = false;
	config.desync_detection = true;
	gekko_start(sSession, &config);
	gekko_net_adapter_set(sSession, &sUdpAdapter);

	/* Every peer adds the slots in the same order: handle == player slot. */
	for (int p = 0; p < sConfig.players; p++) {
		if (p == sConfig.local) {
			int handle = gekko_add_actor(sSession, GekkoLocalPlayer, nullptr);
			gekko_set_local_delay(sSession, handle, (unsigned char)sConfig.delay);
		} else {
			GekkoNetAddress addr{ &sPeerAddr, sizeof(sPeerAddr) };
			gekko_add_actor(sSession, GekkoRemotePlayer, &addr);
		}
	}
	for (int p = 0; p < 4; p++) {
		syNetInputSetSlotSource(p, kSourceSaved);
	}
	sFrames = sRollbacks = sResimTicks = sStalls = sDesyncs = 0;
	port_log("SSB64 Rollback: session created local=%d port=%d peer=%s delay=%d window=%d scripted=%d\n",
	         sConfig.local, sConfig.port, sConfig.peer.c_str(), sConfig.delay, sConfig.window,
	         sConfig.scripted ? 1 : 0);
	return true;
#endif
}

NetInput LocalInput()
{
	NetInput in{};
	if (sConfig.scripted) {
		/* GekkoNet files local input under (current frame + delay). */
		PortNetInputFrame frame{};
		unsigned tick = syNetInputGetTick() + (unsigned)sConfig.delay;
		if (syNetReplayGetLoadedFrame(sConfig.local, tick, &frame)) {
			in.buttons = frame.buttons;
			in.stick_x = frame.stick_x;
			in.stick_y = frame.stick_y;
		}
		return in;
	}
	PortContPad pads[4] = {};
	osContGetReadData(pads);
	in.buttons = pads[0].button;
	in.stick_x = pads[0].stick_x;
	in.stick_y = pads[0].stick_y;
	return in;
}

void HandleSessionEvents()
{
	int count = 0;
	GekkoSessionEvent **events = gekko_session_events(sSession, &count);
	for (int i = 0; i < count; i++) {
		GekkoSessionEvent *e = events[i];
		switch (e->type) {
		case GekkoPlayerSyncing:
			port_log("SSB64 Rollback: syncing with player %d (%d/%d)\n", e->data.syncing.handle,
			         e->data.syncing.current, e->data.syncing.max);
			break;
		case GekkoPlayerConnected:
			port_log("SSB64 Rollback: player %d connected\n", e->data.connected.handle);
			break;
		case GekkoPlayerDisconnected:
			port_log("SSB64 Rollback: player %d disconnected\n", e->data.disconnected.handle);
			break;
		case GekkoSessionStarted:
			sStarted = true;
			port_log("SSB64 Rollback: session started\n");
			break;
		case GekkoDesyncDetected:
			sDesyncs++;
			if (sDesyncs <= 10) {
				port_log("SSB64 Rollback: DESYNC frame=%d local=%08X remote=%08X (player %d)\n",
				         e->data.desynced.frame, e->data.desynced.local_checksum,
				         e->data.desynced.remote_checksum, e->data.desynced.remote_handle);
			}
			break;
		default:
			break;
		}
	}
}

void HandleGameEvents()
{
	int count = 0;
	GekkoGameEvent **events = gekko_update_session(sSession, &count);
	bool counted_rollback = false;
	for (int i = 0; i < count; i++) {
		GekkoGameEvent *e = events[i];
		switch (e->type) {
		case GekkoSaveEvent: {
			if (!PortRollbackSave(sSaveScratch) || sSaveScratch.size > kStateCapacity) {
				port_log("SSB64 Rollback: save failed at frame %d (%zu bytes)\n", e->data.save.frame,
				         sSaveScratch.size);
				*e->data.save.state_len = 0;
				break;
			}
			std::memcpy(e->data.save.state, sSaveScratch.data, sSaveScratch.size);
			*e->data.save.state_len = (unsigned)sSaveScratch.size;
			*e->data.save.checksum = StateChecksum();
			break;
		}
		case GekkoLoadEvent:
			if (!PortRollbackLoad(e->data.load.state, e->data.load.state_len)) {
				port_log("SSB64 Rollback: load failed at frame %d\n", e->data.load.frame);
			}
			if (!counted_rollback) {
				sRollbacks++;
				counted_rollback = true;
			}
			break;
		case GekkoAdvanceEvent: {
			unsigned frame = (unsigned)e->data.adv.frame;
			unsigned tick = syNetInputGetTick();
			if (frame != tick) {
				port_log("SSB64 Rollback: frame %u advanced at game tick %u\n", frame, tick);
			}
			const NetInput *inputs = (const NetInput *)e->data.adv.inputs;
			for (int p = 0; p < sConfig.players; p++) {
				syNetInputSetSavedInput(p, tick, inputs[p].buttons, inputs[p].stick_x, inputs[p].stick_y);
			}
			if (e->data.adv.rolling_back) {
				PortRunHeadlessTick();
				sResimTicks++;
			} else {
				PortRunRenderedTick();
			}
			break;
		}
		default:
			break;
		}
	}
}

} /* namespace */

extern "C" void port_rollback_on_battle_start(void)
{
	LoadConfig();
	if (sConfig.enabled) {
		sState = State::Pending;
	}
}

extern "C" int port_rollback_session_frame(void)
{
	if (sState == State::Idle) {
		return 0;
	}
	if (!InVSBattle()) {
		EndSession("battle over");
		return 0;
	}
	if (sState == State::Pending) {
		if (!StartSession()) {
			EndSession("start failed");
			return 0;
		}
		sState = State::Running;
	}

	gekko_network_poll(sSession);
	HandleSessionEvents();

	/* Pacing: if this side runs ahead of the peer, hold one frame now and
	 * then so the other side's inputs can catch up. */
	if (sStarted && (sFrames % 8) == 0 && gekko_frames_ahead(sSession) >= 1.0f) {
		sFrames++;
		sStalls++;
		return 1;
	}

	NetInput input = LocalInput();
	gekko_add_local_input(sSession, sConfig.local, &input);
	HandleGameEvents();
	sFrames++;

	if (sStarted && (sFrames % 600) == 0) {
		GekkoNetworkStats stats{};
		gekko_network_stats(sSession, sConfig.local == 0 ? 1 : 0, &stats);
		port_log("SSB64 Rollback: frame=%u tick=%u ping=%ums avg=%.1fms rollbacks=%u resim_ticks=%u stalls=%u "
		         "desyncs=%u ahead=%.2f\n",
		         sFrames, syNetInputGetTick(), stats.last_ping, stats.avg_ping, sRollbacks, sResimTicks, sStalls,
		         sDesyncs, gekko_frames_ahead(sSession));
	}
	return 1;
}
