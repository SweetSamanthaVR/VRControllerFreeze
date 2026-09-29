# Third-party software

VR Controller Freeze does not vendor third-party libraries. The build fetches them from their official repositories and checks each download against a pinned SHA-256.

## OpenVR SDK 2.15.6

Fetched from ValveSoftware/openvr. The app links against its Windows x64 library, and the staged build includes `openvr_api.dll` and Valve's licence as `THIRD_PARTY_OPENVR_LICENSE.txt`. OpenVR remains copyright Valve Corporation under its own licence.

## MinHook 1.3.4

Fetched from TsudaKageyu/minhook and compiled into `driver_vrcontrollerfreeze.dll`. MinHook is copyright Tsuda Kageyu and contributors, with the Hacker Disassembler Engine copyright Vyacheslav Patkov, under the BSD 2-Clause licence. The staged build includes it as `THIRD_PARTY_MINHOOK_LICENSE.txt`.

The project's MIT Licence applies only to its original code and documentation.
