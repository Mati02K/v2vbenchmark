# V2V Benchmark – Setup & Run Guide

Ongoing project to benchmark V2V communication using **UDP, TCP, and IEEE 802.11p (WAVE)** simulations built on **OMNeT++ + INET + Veins + SUMO**.

---

## 1. Prerequisites

Tested on **Ubuntu 20.04 / 22.04**

Install basic dependencies:

```bash
sudo apt update
sudo apt install -y build-essential gcc g++ clang \
    qtbase5-dev qtchooser qt5-qmake qtbase5-dev-tools \
    libxml2-dev zlib1g-dev default-jre \
    cmake git python3 python3-pip
```

---

## 2. Install OMNeT++

1. Download OMNeT++ (recommended: **5.6.2**):

```bash
https://omnetpp.org/download/
```

2. Extract and build:

```bash
tar -xvf omnetpp-5.6.2-src.tgz
cd omnetpp-5.6.2
. setenv
./configure
make -j$(nproc)
```

3. Verify:

```bash
omnetpp
```

---

## 3. Install SUMO

```bash
sudo apt install -y sumo sumo-tools sumo-doc
```

Verify:

```bash
sumo --version
```

---

## 4. Install INET

1. Download INET (recommended: **INET 4.4.x**):

```bash
git clone https://github.com/inet-framework/inet.git
cd inet
git checkout v4.4.2
```

2. Build INET:

```bash
. ~/omnetpp-5.6.2/setenv
make makefiles
make -j$(nproc)
```

---

## 5. Install Veins

1. Clone Veins:

```bash
git clone https://github.com/sommer/veins.git
cd veins
```

2. Build Veins:

```bash
. ~/omnetpp-5.6.2/setenv
./configure
make -j$(nproc)
```

> **Note:** Veins automatically links against SUMO using TraCI.

---

## 6. Import Projects into OMNeT++

1. Open OMNeT++ IDE:

```bash
omnetpp
```

2. Import the following as **Existing Projects**:

* `inet`
* `veins`
* `v2v-benchmark` (this repository) you can just clone it

3. Ensure **Project References**:

   * `v2v-benchmark` → depends on `inet` and `veins`

---

## 7. Running SUMO with TraCI Port (Required)

Veins connects to SUMO via **TraCI**, so SUMO **must** be started with a fixed port that matches the one configured in `omnetpp.ini`.
### Important

### Run SUMO with GUI

```bash
sumo-gui -c scenario.sumocfg --remote-port 9999
```

### Run SUMO Headless
```bash
sumo -c scenario.sumocfg --remote-port 9999
```


* **Start SUMO first**, then run the OMNeT++ simulation.
* Only **one SUMO instance per port** can run at a time.


---

## 8. Running Simulations (OMNeT++)

Each protocol has its **own simulation folder** and `omnetpp.ini`.

run directly from the **OMNeT++ IDE** in simulations folder:

* Right-click `omnetpp.ini`
* **Run As → OMNeT++ Simulation**

---

## Info

I am maintaining this link to add more info about the project and progress :- 
https://docs.google.com/presentation/d/1rOWyffNRj3yZXhFhQUPCryBkdokQcVGUCKkBrsEKTVc/edit?usp=sharing

