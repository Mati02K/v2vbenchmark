# V2V RAFT Intersection Benchmark

Autonomous vehicle intersection coordination benchmark implementing **RAFT consensus** over two wireless transports:

- **UDP** — IEEE 802.11a via INET framework
- **WAVE** — IEEE 802.11p via Veins framework

Compares throughput, latency, fallback rate, and message overhead across 4, 8, and 16 vehicle scenarios with optional priority-vehicle scheduling.

**Stack:** OMNeT++ 5.6.2 · INET 4.4.x · Veins · SUMO 1.8.0 · Python 3.12

---

## Workspace Layout

All components live as siblings under a single workspace directory (e.g. `~/omnetpp-workspace/`):

```
omnetpp-workspace/
├── inet/          ← INET 4.4.x (UDP/TCP/WiFi models)
├── veins/         ← Veins (WAVE/802.11p + TraCI)
└── benchmark/     ← this repository
```

The Veins library also lives at `~/veins/` (the path baked into the Makefile — see [Build](#build)).

---

## 1. Prerequisites

Tested on **Ubuntu 22.04 / 24.04**.

```bash
sudo apt update
sudo apt install -y \
    build-essential gcc g++ clang \
    qtbase5-dev qtchooser qt5-qmake qtbase5-dev-tools \
    libxml2-dev zlib1g-dev default-jre \
    cmake git python3 python3-pip \
    libssl-dev \
    sumo sumo-tools sumo-doc

pip3 install numpy matplotlib
```

Verify SUMO:

```bash
sumo --version   # should print: Eclipse SUMO sumo Version 1.8.0 (or later)
```

---

## 2. Install OMNeT++ 5.6.2

OMNeT++ 5.6.2 is required — the codebase uses OMNeT++ 5.x APIs and does not compile against 6.x.

```bash
# Download from https://omnetpp.org/download/older/
tar -xvf omnetpp-5.6.2-src.tgz
cd omnetpp-5.6.2
. setenv
./configure
make -j$(nproc)
```

Add OMNeT++ to your shell profile so `setenv` is available:

```bash
echo '. ~/omnetpp-5.6.2/setenv' >> ~/.bashrc
source ~/.bashrc
```

Verify:

```bash
which opp_makemake   # should print a path inside omnetpp-5.6.2/bin/
```

---

## 3. Install INET 4.4.x

INET provides the UDP/IP and IEEE 802.11a radio stack used by the UDP transport.

```bash
mkdir -p ~/omnetpp-workspace && cd ~/omnetpp-workspace
git clone https://github.com/inet-framework/inet.git
cd inet
git checkout v4.4.2

. ~/omnetpp-5.6.2/setenv
make makefiles
make -j$(nproc) MODE=release
```

---

## 4. Install Veins

Veins provides the IEEE 802.11p (WAVE) radio stack and the TraCI interface that connects OMNeT++ to SUMO at runtime.

```bash
cd ~   # Veins must live at ~/veins/ — the Makefile has this path hardcoded
git clone https://github.com/sommer/veins.git veins
cd veins

. ~/omnetpp-5.6.2/setenv
./configure
make -j$(nproc) MODE=release
```

> The benchmark Makefile references Veins at `VEINS_PROJ=/home/<user>/veins`.
> If your username differs, open `src/Makefile` and update `VEINS_PROJ`.

### veins_launchd

`veins_launchd` is the Veins helper that manages SUMO processes for each simulation run. It must be running before any simulation starts:

```bash
# Install dependency
pip3 install --user pyzmq

# Start the launcher (keep this terminal open)
cd ~/veins
python3 ./bin/veins_launchd -vv -c /usr/local/bin/sumo-gui
```

This launches SUMO-GUI automatically for each simulation run and listens on port 9999.

For headless (CI/server) use:

```bash
python3 ./bin/veins_launchd -vv -c /usr/local/bin/sumo
```

---

## 5. Build the Benchmark

Every new terminal session requires OMNeT++ on the PATH:

```bash
export PATH="$HOME/omnetpp-5.6.2/bin:$PATH"
export LD_LIBRARY_PATH="$HOME/omnetpp-5.6.2/lib:$LD_LIBRARY_PATH"
```

Build the release binary (used by all run scripts):

```bash
cd ~/omnetpp-workspace/benchmark/src
make MODE=release all          # → src/benchmark
```

Debug binary (for crash investigation only):

```bash
make MODE=debug all            # → src/benchmark_dbg
```

Clean:

```bash
make MODE=release clean
make MODE=debug   clean
```

### What gets compiled

| Component | Location |
|---|---|
| RAFT consensus library | `lib/raft/` (willemt/raft, statically compiled) |
| Crypto / QC signing | `lib/crypto/CryptoAuth.cc` (OpenSSL) |
| Protocol logic | `src/raft/RaftAppBase.cc` + split files |
| UDP subclass | `src/raft/UdpRaftApplication.cc` |
| WAVE subclass | `src/raft/WaveRaftApplication.cc` |
| OMNeT++ message types | `src/raft/*.msg` (auto-generates `*_m.cc/h`) |

---

## 6. Running Simulations

### Start veins_launchd first

```bash
# Verify it is already running:
ss -tlnp | grep 9999

# If not running, start it:
cd ~/veins && python3 ./bin/veins_launchd -vv -c /usr/local/bin/sumo-gui
```

### Run the benchmark

```bash
cd ~/omnetpp-workspace/benchmark

# 10 iterations, laneLeaders mode (default), priority vehicle rotates each run
./benchmark.sh 10

# 10 iterations, allVehicles mode (all vehicles participate in RAFT)
./benchmark.sh 10 allVehicles

# 10 iterations, nopriority (no priority vehicle — all vehicles treated equally)
./benchmark.sh 10 laneLeaders nopriority
./benchmark.sh 10 allVehicles nopriority
```

Each run covers **4, 8, and 16 vehicles × UDP + WAVE** automatically (6 scenario combinations per benchmark call).

Priority and nopriority results are saved to separate directories and never overwrite each other.

### Simulation directories

| Vehicles | Directory |
|---|---|
| 4  | `simulations/simple_intersection_4/` |
| 8  | `simulations/simple_intersection_8/` |
| 16 | `simulations/simple_intersection/`   |

Each contains `omnetpp_udp.ini`, `omnetpp_wave.ini`, and the SUMO route/network files.

---

## 7. Results

Results land under `results/` with the following naming convention:

```
results/
  simple_udp_4veh_laneLeaders/
    run_1/
      raft_results.json     ← per-vehicle metrics
      console.log           ← simulation stdout
    run_2/ ...

  simple_raftwave_4veh_laneLeaders/   (WAVE equivalent)
  simple_udp_4veh_allVehicles/        (allVehicles cluster mode)
  simple_udp_4veh_laneLeaders_nopriority/   (nopriority run)
  ...
```

---

## 8. Plotting

```bash
cd ~/omnetpp-workspace/benchmark

# Plots for a single cluster mode
python3 plot_comparison.py --simple --mode laneLeaders
python3 plot_comparison.py --simple --mode allVehicles
python3 plot_comparison.py --simple --mode laneLeaders_nopriority
python3 plot_comparison.py --simple --mode allVehicles_nopriority

# Side-by-side comparison: laneLeaders vs allVehicles
python3 plot_comparison.py --simple --compare-modes
```

Output PNGs are saved directly to `results/`:

| File | Contents |
|---|---|
| `throughput_<mode>.png` | Vehicles/second crossing rate |
| `fallbacks_<mode>.png` | Fraction of vehicles that fell back from RAFT |
| `messages_<mode>.png` | Messages sent per vehicle |
| `cdf_<mode>.png` | CDF of total wait time (stopped → passed) |
| `ambulance_<mode>.png` | Priority vs normal vs all vehicle wait time |
| `throughput_compare.png` | laneLeaders vs allVehicles comparison |
| `fallbacks_compare.png` | laneLeaders vs allVehicles comparison |
| `messages_compare.png` | laneLeaders vs allVehicles comparison |
| `cdf_compare.png` | laneLeaders vs allVehicles comparison |

---

## 9. Cluster Modes

| Mode | Description |
|---|---|
| `laneLeaders` | Only the front vehicle of each lane joins RAFT (4-node cluster). Followers receive the crossing schedule via broadcast. |
| `allVehicles` | Every vehicle at the intersection joins the RAFT cluster. Larger cluster, more overhead, more resilience. |

---

## 10. Architecture Overview

All shared protocol logic lives in `RaftAppBase`. Two thin subclasses differ only in transport:

```
RaftAppBase  (abstract)
├── UdpRaftApplication   — UDP / INET / VeinsInetApplicationBase
└── WaveRaftApplication  — WAVE / Veins / DemoBaseApplLayer
```

Each subclass implements exactly three pure virtuals:

```cpp
virtual void sendRaftUnicast(int targetVehicleId, uint8_t msgType,
                             const std::vector<uint8_t>& data) = 0;
virtual void sendRaftBroadcast(uint8_t msgType,
                               const std::vector<uint8_t>& data) = 0;
virtual double getDistanceToJunction() = 0;
```

### Protocol phases

1. **Discovery** — vehicles broadcast beacons, build peer list, stop at intersection
2. **RAFT election** — willemt/raft runs RequestVote; one leader is elected
3. **Decision** — leader collects vehicle proposals, computes crossing schedule, replicates via RAFT log, distributes via quorum-certificate broadcast
4. **Execution** — vehicles move in batches; each vehicle broadcasts `VEHICLE_LEFT` on crossing; next batch advances when current batch clears
5. **Metrics** — each vehicle appends its JSON record to `raft_results.json`

---

## 11. Troubleshooting

**Build fails: "No rule to make target"**
The linter may have reverted the Makefile. Verify these lines exist in `src/Makefile`:
```makefile
RAFT_DIR   = ../lib/raft
CRYPTO_DIR = ../lib/crypto
LIBS += -lssl -lcrypto
```

**Simulation hangs / no output**
`veins_launchd` is not running or SUMO crashed. Check:
```bash
ss -tlnp | grep 9999        # must show a listener
pgrep -a python3 | grep veins_launchd
```

**High fallback rate**
Check `results/<name>/run_N/console.log` for:
- `FALLBACK ACTIVATED` — vehicle gave up on RAFT (election timeout exceeded `maxFailedElections_`)
- `VEHICLE_LEFT TIMEOUT` — dropped VEHICLE_LEFT message (500 ms penalty per batch)
- `FORM_CLUSTER` — how many vehicles joined the cluster (small cluster = no quorum)

**OMNeT++ not found after reboot**
```bash
export PATH="$HOME/omnetpp-5.6.2/bin:$PATH"
export LD_LIBRARY_PATH="$HOME/omnetpp-5.6.2/lib:$LD_LIBRARY_PATH"
```
