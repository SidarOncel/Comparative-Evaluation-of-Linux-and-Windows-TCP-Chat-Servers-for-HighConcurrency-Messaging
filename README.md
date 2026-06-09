# High-Concurrency TCP Server Performance Analysis

## Overview

This project investigates the scalability and performance characteristics of high-concurrency TCP servers on Linux and Windows operating systems.

A real-time messaging (chat) system will be implemented in C and used as an experimental platform to compare:

* Linux `epoll`
* Windows IOCP (I/O Completion Ports)

The objective is to evaluate how different operating-system networking architectures behave under increasing numbers of concurrent TCP connections.

---

## Research Objectives

The project aims to:

* Implement a TCP-based chat server in C
* Develop a custom load generator
* Simulate up to 1000 concurrent clients
* Measure latency and throughput
* Measure CPU and memory consumption
* Compare Linux and Windows event-driven networking models
* Identify scalability bottlenecks and failure thresholds

---

## Technologies

### Programming Language

* C (C17 standard)

### Operating Systems

* Ubuntu Linux (Primary Development Platform)
* Windows 11 (Comparison Platform)

### Networking APIs

Linux:

* socket()
* bind()
* listen()
* accept()
* recv()
* send()
* epoll_create1()
* epoll_ctl()
* epoll_wait()

Windows:

* Winsock2
* CreateIoCompletionPort()
* GetQueuedCompletionStatus()

---

## Required Tools

### Compiler

Verify installation:

```bash
gcc --version
```

Expected:

```text
gcc (Ubuntu) 13.x or newer
```

Install:

```bash
sudo apt update
sudo apt install build-essential
```

---

### Git

Verify:

```bash
git --version
```

Install:

```bash
sudo apt install git
```

---

### Netcat

Used for manual client testing.

Verify:

```bash
nc -h
```

Install:

```bash
sudo apt install netcat-openbsd
```

Example:

```bash
nc localhost 8080
```

---

### Network Utilities

Install:

```bash
sudo apt install net-tools
```

Useful commands:

```bash
ifconfig
netstat
```

---

### Modern Socket Inspection Tool

Install:

```bash
sudo apt install iproute2
```

Useful command:

```bash
ss -tulnp
```

---

### Build System

Install:

```bash
sudo apt install make
```

Verify:

```bash
make --version
```

---

### Packet Analyzer 

Wireshark may be used during later testing phases.

Install:

```bash
sudo apt install wireshark
```

---

## Project Structure

```text
network-comparison/
│
├── docs/
│   ├── architecture.md
│   └── proposal.pdf
│
├── server/
│   ├── server.c
│   └── Makefile
│
├── client/
│   └── client.c
│
├── load_generator/
│   └── generator.c
│
├── scripts/
│
├── results/
│
└── README.md
```

## Expected Outcomes

The project is expected to demonstrate:

* Practical TCP server implementation
* Event-driven network programming
* Scalability characteristics of Linux and Windows
* Performance trade-offs between epoll and IOCP
* Bottlenecks encountered in high-concurrency network services

---

## Author

Sidar B. Oncel

### Purpose:
Computer Networks Course Project