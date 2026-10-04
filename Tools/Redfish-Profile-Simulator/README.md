Copyright 2016-2018 Distributed Management Task Force, Inc. All rights reserved.

# Redfish Profile Simulator

## About

***Redfish Profile Simulator*** is a Python 3 simulator of the "simple monolithic server" feature profile.

* A simple, minimal Redfish Service
* For a monolithic Server
* Aligned with: OCP Remote Machine Management Spec feature set

### Description

* Based on flask
* Initial resources are loaded from a catfish mockup into python dictionary structures
    * After that, data is read/patched... to the dictionaries
* Supports BasicAuth, as well as Redfish Session Auth  (for one session, one user)
* Uses:
    * easy to add new URIs for testing a client
    * easy to tweak behavior or add bad responses to test a client
    * allows testing of authentication -- which current mockup servers dont do
    * easy to insert print statements in service to see if data coming across good..etc

### Current Limitation:

* supports a single user/passwd and token
* Basic auth: `admin` / `pwd123456`; session login: `root` / `password123456`
* The authToken for Session Auth is: 123456SESSIONauthcode
* Supports HTTP, and HTTPS when both certificate and key are provided
* with redfishtool, use options: redfishtool.py -r127.0.0.1:5000 -u admin -p pwd123456 -S Never <subcmd>

## Usage

* ` python redfishProfileSimulator.py [options]`
* `[Options]`:

        -V,  --Version,--- the program version
        -h,  --help,   --- help
        -H<hostIP>,  --Host=<hostIp>   --- host IP address. dflt=127.0.0.1
        -P<port>,--Port=<port> --- the port to use. dflt=5000
        -p<profile_path>, --profile=<profile_path>   --- the path to the Redfish profile to use. dflt="MockupData/SimpleOcpServerV1" beside the script

## Implementation

* The simulation includes an http server, RestEngine, and dynamic Redfish datamodel.
* You can GET, PATCH,... to the service just like a real Redfish service.
* Both Basic and Redfish Session/Token authentication are supported
    * for a single user/passwd and token
    * Basic auth: `admin` / `pwd123456`; session login: `root` / `password123456`
    * The authToken for Session Auth is: 123456SESSIONauthcode
    * these can be changed by editing the redfishURSs.py file---will make dynamic later.
* The http service and Rest engine is built on Flask, and dependencies require a current Python 3 release
* The data model resources are "initialized" from the SPMF "SimpleOcpServerV1" Mockup.
    * and stored as python dictionaries
    * then the dictionaries are updated with patches, posts, deletes.
* The program can be extended to support other mockup \"profiles\".
* By default, the simulation runs on localhost (127.0.0.1), on port 5000.
    * These can be changed with CLI options: -P<port> -H <hostIP>  | --port=<port> --host=<hostIp>

## Simple OCP Server V1 Mockup Description

* A Monolithic server:
    * One ComputerSystem
    * One Chassis
    * One Manager

* Provides basic management features aligned with OCP Remote Machine Management Spec 1.01:
    * Power-on/off/reset
    * Boot to PXE, HDD, BIOS setup (boot override)
    * 4 temp sensors per DCMI (CPU1, CPU2, Board, Inlet)
    * Simple Power Reading, and  DCMI Power Limiting
    * Fan Monitoring w/ redundancy
    * Set asset tag and Indicator LED
    * Basic inventory (serial#, model, SKU, Vendor, BIOS ver…)
    * User Management
    * BMC management: get/set IP, version, enable/disable protocol

* What it does NOT have -- that the Redfish 1.0 model supports
    * No PSUs in model  (RMM spec did not include PSUs)
    * No ProcessorInfo, MemoryInfo, StorageInfo, System-EthernetInterfaceInfo
    * No Tasks
    * JsonSchema and Registries collections left out (since that is optional)
    * No EventService--Remote Machine Management spec used basic PET alerts
    * Uses only the pre-defined privileges and roles

## TO DO

Some limitations to be extended in current implementation

* Auth supports a single hard-coded username, password, and AuthToken, although the protocol is 100% compliant with respect to testing clients trying to authenticate
    * Basic auth uses `admin` / `pwd123456`; session login uses `root` / `password123456`
    * ex with Session Auth, you just use the hard coded AuthToken
* adding and deleting users not implemented--has 3 or 4 users predefined
* accountService properties can be written, but failed logins, lockouts, etc is not implemented
* system log not implemented yet

## Linux quick start

Use Python 3.10+ and an isolated virtual environment. The commands below are
run from the repository root; no root privileges, BMC, or global package
installation is needed. The automated tests were verified with Python 3.12.14,
Flask 3.0.0, and Werkzeug 3.1.9.

```sh
python3 -m venv /tmp/redfish-simulator-venv
/tmp/redfish-simulator-venv/bin/python -m pip install -r Tools/Redfish-Profile-Simulator/requirements.txt
/tmp/redfish-simulator-venv/bin/python Tools/Redfish-Profile-Simulator/redfishProfileSimulator.py -H 127.0.0.1 -P 5000
```

The default is `127.0.0.1:5000`, and the default profile is located relative to
the simulator script, so launching from another working directory also works.
An explicit `-p` / `--profile` path is relative to the caller's working directory.
Both legacy `--Host` / `--Port` and lowercase `--host` / `--port` are accepted.
The previously documented `--profile_path` spelling is also accepted.
Short options work with or without a space before the value. Invalid options,
ports, missing/malformed profiles, or incomplete certificate/key pairs return a
nonzero exit status. Paths containing spaces should be quoted in your shell.

Verify the service using the bundled demonstration credentials:

```sh
curl http://127.0.0.1:5000/redfish/v1/
curl -u admin:pwd123456 http://127.0.0.1:5000/redfish/v1/Systems
```

HTTPS can use an unprivileged port. Supply your test certificate and key together:

```sh
/tmp/redfish-simulator-venv/bin/python Tools/Redfish-Profile-Simulator/redfishProfileSimulator.py -H 127.0.0.1 -P 8443 -C /path/to/cert.pem -K /path/to/key.pem
```

`--Cert` / `--Key` and their lowercase aliases are supported. Port 443 still
requires the pair. The simulator never generates or installs certificates, and
clients must trust your test certificate. Existing Windows Python launch commands
continue to work; virtual-environment executables on Windows are in `Scripts`.

This is a development service with hard-coded public test credentials, a fixed
authentication token, and incomplete Redfish behavior. Keep it on loopback or a
private isolated test network. Binding another interface is an explicit `-H`
choice. Do not expose it publicly or send production credentials to it. Flask's
debugger and automatic reloader are disabled. Authentication headers and session
login passwords are not printed by the authentication handlers.

## Automated tests

```sh
/tmp/redfish-simulator-venv/bin/python -m unittest discover -s Tools/Redfish-Profile-Simulator/tests -v
```

The standard-library `unittest` suite needs only `requirements.txt`. It loads a
temporary copy of the profile, binds only IPv4 `127.0.0.1` on operating-system
allocated ephemeral ports, and shuts down its HTTP/HTTPS servers on completion.
It does not need Unix-domain sockets or a physical BMC, does not modify bundled
mockups, and does not run an emulated UEFI firmware image. HTTPS tests create a
short-lived local test certificate and validate it with an isolated client trust
context. CLI launch arguments, error exit status, Windows-compatible path logic,
service discovery, Basic/token authentication, inventory resources, PATCH,
ETag header changes, session response shape, and reset actions are covered.
Windows itself has not been exercised by these Linux tests.

### Protocol limitations preserved by these changes

- Basic auth (`admin` / `pwd123456`) and session login
  (`root` / `password123456`) intentionally retain their existing different credentials.
- Session login returns a fixed token. Logout returns 204 but does not revoke it.
- System PATCH updates the ETag header. Resource routes currently bypass the
  legacy conditional-response hook: `If-Match` does not protect updates and
  `If-None-Match` does not return 304. Tests characterize this gap separately from
  testing the legacy hook's compatibility with modern Flask/Werkzeug.
- Mutations are in memory and disappear when the process restarts. This is not a
  full schema validator; not every malformed request or unsupported URI has a
  standards-compliant error response.
- Tests cover the listed routes, not every optional resource or action. Passing
  this suite does not establish Redfish conformance or firmware network support.
