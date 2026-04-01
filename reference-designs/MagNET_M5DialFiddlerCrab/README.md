# Overview

The goal of this prototype is to build a platform for MAGNet "hive" AI.  At the core, you will useually need a capable controller for business logic, secure delegation of access, and UX.  However, it's possible everything in the network is fairly autonomous and without UX.

This prototype will create a UI that is capabile of utilizing other members of the hive.  Modeling biology, natural systems for insects often organize into roles related to work or biological function.  In our demo, we will scratch the surface on the surface.  We allow hive intelligence bits to get deployed, possibly self modify, and possibly transfer between these Digital Biologic self-Organized  Telepathic Sentients (DBOTS).  

Digital: they are running modern RISC architecture
Biologic: code can self replicate
Organized: they operate in a strict set of rules and heirarchy
Telepathic: they can read each others "minds", and perhaps some day they can read ours
Sentients: they operate without intervention from their makers.  They will try to make decisions to fulfill goals.

There will be multiple phases of development.  Each phase will attempt to progressively create newer capabilities. 
he focus of this design prototype is to exercise a concept of a biologic node that can self replicate, self modify, receive upgrades from "the hive", and utilize shared memory.  

## Design

OTA is a core ability we wish to enable is for nodes to be able to change their roles and capabilities to do work.  In our demonstration prototype, we will use an M5Stack Dial https://shop.m5stack.com/products/m5stack-dial-esp32-s3-smart-rotary-knob-w-1-28-round-touch-screen based on a core base of software starting from this code: https://github.com/Pharkie/M5StackDial-m5gfx-demo .  This code doesn't have any networking.  We assume this code will be altered to add networking via WIFI or BLE.  For this prototype, we will just use WiFi.  The code will need to be configurable via BLE to connect to WIFI.  

At the core, the "biologic" code is only going to contain logic, and we want to implement a Forth dialect with an FFI that has access to core ESP-IDF code.  We otherwise treat the biologic layer as a common abstraction for code that can be updated via the network, validated using hashing, and versioned.   We want to design a unique feature of the biologic abstraction of "hive mind" which is like shared memory among all nodes.  Because this isn't really local memory, though possibly some portions could live in local memory, it would subscribe to uddates to memory of interest.  Memory would be defined using Named Data Networking (or Content Centric Networking).  The device wouldn't really know the difference if it was local or not, just knowing the freshness of its cache and state.  The hive acts as an extension of the whatever main entity is in charge, often a "queen" or "ruler" of the hive.  In our demo, the "ruler" is just the human in charge of a demo and who has physical control of nodes.

For our demo we want to be able to hot swap roles of the node, which will download from the network, and will get validated, and executed.  We will implement 10 simple "biologic" date upgrades with different functions.  The flutter application will really act as a gateway to some LLM, and possibly be in communication with a user on the other end, though it might be communicating unattended.

### Requirements

- R1: Uses an ESP32-IDF platform with Platform.io
- R2: uses a core starter project with some graphical display and touch UI
- R3: On activation, it turns on and if not configured, will start advertising as MagNET-biologic-MAC
- R4: On connection via BLE, another device (a flutter ble app for mac or linux) will be able to talk directly to this node and configure the wifi
- R5: Using mdns, or some other suitable P2P protocol that is small or multicast, the node should find a "ruler" operating on a known port that is able to validate that it posesses some shared secret, and can establish a session.
- R6: A node gets can request to join the hive, and is accepted or rejected based on concesus of the hive.
- R7: A node if accepted, can request a role in the hive. 
- R8: A role bestows a specialization of skills, knowledge, and goals upon a node.  A role may be a super role that encompansses other roles.
- R9: Upon receiving a role, the node effectively will download a new script of instructions.
- R10: Before execution and loading of a new role, a node must validate the authenticity of the data and perform crc integrity check, check the author/signer, and version check.
- R11: if the role instructions are are ok and pass validation, the version is  verified.  Generally upgrades will operate sequentially unless the hive requires a downgrade or non-standard versioning (i.e. a development build vs official release, or a downgrade/rollback).  
- R12: upon validating a version, the node will execute its role
- R13: With a new role, a node may receive instructions to update its physical interfaces (LEDs, screens, or possibly reconfigure an actual physical characteristic).  This is known as installation.
- R14: With a new role, a node may receive a new configuration.  It is up to the role and the hive rules to determine whether
- R15: The state machine is now executing the role.  This is known as hive work.
- R16: At any time, the node may request shared memory of the hive.  Since each node has finite memory, it may be that there are nodes in the network that have longer memory, or that each node is responsible to keep a particular piece of memory alive.  It will be queried transparently to the node, using something like Named Data Networking (see also CCN).  
- R17: A node may reponsible to reply to have shared memory requests.
- R18: A node can talk P2P between nodes using a yet to be determined protocol.  For now we will assume this operation is something addressed as /chat/PEERID (in Named Data Networking parlance)
 
Since this is design concept v1 we expect there to be future ideas to enhance and modify this design.

Roles for Nodes in this demo design concept will be 10 different roles:

- Role 1: Ruler.  If No Ruler is found, any node can request nomination to Ruler.  Ruler should display a Fiddler Crab on its screen if available.
- Role 2: Worker: It receives commands from the Ruler via /chat , and carries out the tasks.  Should display a robot with a pick axe.
- Role 3: Parrot: A parrot only echoes commands received.  Should display a robot parrot.
- Role 4: Scribe: A scribe's only job is to save data to its internal memory and recall it from shared memory if asked.  Should display an old scholar with a stone tablet.
- Role 5: Beeper: A beeper's job is to light up or make a noise when asked to do so.  Should display a 1980s style beeper.
- Role 6: Warrior: A warrior will attack unwanted entitites who attempt to invade the network.  Should display a robot warrior with a spear.  A ruler can dicate the attacks of a warrior.
- Role 7: Spy: A spy should listen to all activity and notify the ruler of new nodes
- Role 8: Pet: Just a cute animal and belongs to the ruler.  It has a special skill of barking at strangers.  Should display a cute pet.
- Role 9: ML PhD: A PhD that designs modifications to roles and distributes them for review by the ruler and scribe, eventually upgrading the hive.
- Role 10: Spawn: Any new member of this tribe will first be a spawn.  It will have no responsibilities other than to learn anything it needs from the scribe and other roles before taking a new role.

## Development


### Phase 0 : Janet Language Port to ESP-IDF

To get comfortable with understanding how to integrate Janet language https://janet-lang.org/, which is an embeddable Lisp style language, into the ESP-IDF for targets ESP32, ESP32-S3, ESP32-C3, as well as ESP32-C6.  We would need the new version to build taking an embedded scripting approach.  The goal will be to install on any of these targets, and re-run the full test suite from the REPL as a sample test application.  We will call this port EspJanet.

**Status**: The EspJanet project lives in the `EspJanet/` subdirectory.  It builds for ESP32-S3, ESP32-C3, and ESP32 classic via PlatformIO with the `espidf` framework.  Janet v1.41.2 amalgamation is compiled as an ESP-IDF component with embedded-friendly config (single-threaded, no EV/net/FFI/threads, reduced OS).  A REPL over UART is implemented with memory stats reporting.  This is the first known port of Janet to ESP32.  See `EspJanet/` for details.

### Phase 1 : Janet Language Port of the existing source

**Status**: SKIPPING because Janet won't work on most of the existing small targets.

The existing project that exists in this repository, in the subfolder M5StackDial-m5gfx-demo, is a nice implementation of touch interface with graphics, and utilizes nearly all of the features of an M5Stack Dial, other than networking.  Take the work from phase 0, and re-implement M5StackDial-m5gfx-demo in the Jan.  Let's call the new application M5StackDial-m5gfx-demo-ESPJanet.  Put it in a new project directory.  The validation will be that it compiles and is successfully installed, and that it runs in the roughly the same manner as the original C++ code.  The expectation is this will be using the FFI via Janet.

### Phase 2: ESP32FORTH Port to ESP-IDF

To get comfortable with FORTH as an alternative language, please see: https://esp32forth.appspot.com/ESP32forth.html and evaluate this as a porting candidate into the ESP-IDF for targets ESP32, ESP32-S3, ESP32-C3, as well as ESP32-C6.  We would need the new version to build taking an embedded scripting approach.  The goal will be to install on any of these targets, and re-run the full test suite from the REPL as a sample test application.  We will call this port ESPIDFORTH.

**Status**: The ESPIDFORTH project lives in the `ESPIDFORTH/` subdirectory.  It builds and runs on ESP32-S3, ESP32-C3 (tested on hardware), ESP32-C6, and ESP32 classic via PlatformIO with the `espidf` framework.  A stub Forth interpreter implements the core ANS Forth word set (arithmetic, stack ops, comparisons, logic, colon definitions, variables, constants, and control flow).  A built-in test suite of 47 assertions plus 8 FFI tests with per-test microsecond timing is available via the `test` and `test-ffi` words at the REPL.  The Forth engine is packaged as a **self-contained ESP-IDF component** (`ESPIDFORTH/components/forth/`) that can be dropped into any ESP-IDF or PlatformIO project — just copy the directory, add as a git submodule, or use the ESP Component Manager.  The full ESP32forth v7.0.8.0 source is preserved for the next phase of porting.  See `ESPIDFORTH/README.md` for full details.

### Phase 3: ESPIDFORTH Port of the existing source

The existing project that exists in this repository, in the subfolder M5StackDial-m5gfx-demo, is a nice implementation of touch interface with graphics, and utilizes nearly all of the features of an M5Stack Dial, other than networking.  Take the work from Phase 2, and re-implement M5StackDial-m5gfx-demo in FORTH.  Let's call the new application M5StackDial-m5gfx-demo-ESPIDFORTH.  Put it in a new project directory.  The validation will be that it compiles and is successfully installed, and that it runs in the roughly the same manner as the original C++ code.  The expectation is this will be using an FFI to interact with ESP-IDF libraries in C.

### Phase 4: Implementation of Design section

TODO.
