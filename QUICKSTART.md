# Waveserver Mini Quickstart

This quickstart keeps the current challenge code exactly as-is and focuses on a smooth local workflow.

## Prerequisites
- macOS or Linux shell
- gcc
- make
- python3

## 1) One-time setup
```bash
make setup
```

## 2) Build
```bash
make
```

## 3) Start backend services
```bash
make run
```
This starts:
- port_manager (UDP 5001)
- conn_manager (UDP 5002)
- traffic_manager (UDP 5003)

## 4) Open CLI (new terminal)
```bash
./cli
```

## 5) Run tests
```bash
make test
```
Note: failing tests are expected right now because challenge bugs/features are intentionally unfinished.

## 6) Stop services
```bash
make stop
```

## Useful cleanup
```bash
make clean      # remove binaries
make cleanall   # remove binaries + wsmini.log + dSYM bundles
```
