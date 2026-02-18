# Overview

The "Internet Of Things", popularly known as IoT, presented a unique way of looking at the nature of data, connectivity, secruity, and standards around small low power compute devices as they relate to people and the larger world.  "The WebXR of things" as a phrase expresses the ability to explore devices and places and even people,  in a spatial mixed reality sense.  This concept is expressed as a desire to allow XR to become the default interface to devices, places, people, and data.  The caveat to this statement, is, the user should have the option to "own" this tech stack, as opposed to the past 20+ years of computing, which largely operated through a gatekeepers deciding matters of privacy, ownership of copyright over data, collaboration with government (or not) and the right to monestize data.  Open standards are desired as an approach to deliver a stack owned by the consumer, self hosted, paid SaaS hosted, or free with rights granted to the owner of the stack.  This paper will also address the caveats and challenges in the current tech stacks for delivering the "WebXR of Things".  In this discussion, we will focus on the "hyperlocal" scale.  This implies the immediate area, within sight of the a user, within short range, within walking distance, and not larger scale.

## The Problem 

The promise of IoT offered the ability to introspect environments and communicate with a small but powerful compute devices living at the edge of network environments.  The reality has been, walled gardens dominate IoT.  Until recently, large gatekeepers have tried to avoid standards.  We now have an opportunity with strong OSS comunities to push the industry to adopt open standards, or use defacto high quality open source.  For WebXR, we don't yet have a way to explore the environment in a "hyperlocal" context, to instrospect IoT devices, people, places, and data or state.  There is currently no standard way to grab a popular WebXR device (an ipad, a Meta Quest 3, an Apple Vision Pro, Snap Spectacles) and "join" the local area / room / space to explore, introspect, and control services provided by devices.  Every other ecosystem has stronger capabilities.  TVs have more capabilities that WebXR devices in that they have native ways to explore services on a network and exploit them for positive use cases.  For WebXR what is needed is exploration of approaches to service discovery beyond the URL.  Current mechanisms require all communications to flow through "websites".  APIs to integrate with devices that might be worn or just inches or meters away are not available.  Hence, the exploration of "WebXR of Things" (or simply, the XrOT)  

## Inspirations

Attribution: Adam Varga (design concept)
<img width="688" alt="Image" src="https://github.com/user-attachments/assets/17402d55-ae57-4a2f-ad77-8cb0d7d334ab" />
<img width="688" alt="Image" src="https://github.com/user-attachments/assets/57e6e5b2-767a-4ce2-bd22-e55e133ef6a2" />

Credit: https://www.linkedin.com/posts/dmvrg_unity-m5stack-arduino-activity-7341335884333531138-oHRI/?rcm=ACoAAAAIENgBAB7U7RX_Hl-tsAvNwMh3WO-qm4E

## References

- IoT:  https://en.wikipedia.org/wiki/Internet_of_things
- WebXR Specficiation: https://www.w3.org/TR/webxr/

## Proposed Solution(s)

We propose to build a standard open source platform to allow WebXR users to explore the "hyperlocal" context (the area in the immediate vicinity around a user) via WebXR, exploring the personal range (wearables, or devices within reach of the user), roomscale range (within a room), or network scale (within the network), or as is always possible, Internet scale.  Users should be able introspect: services, data, devices, people (yes), and state within a hyperlocal  context, with the ultimiate goal being: this can work easily on different HMDs and Mixed Reality Glasses.

### Requirements

- R1: Cross Platform
- R2: Open Platform
- R3: Royalty Free Distribution possible
- R4: Documented and versioned API specifications
- R5: License free conformance suite available
- R6: An Open Source Reference Implementation is available
- R7: Self hosted
- R8: Works in a WebXR compatible context (implies WebXR secure context)
- R9: Doesn't require access front facing HMD / Mix Reality Glasses cameras to function
- R10: User can discover services
- R11: User is continuously aware of state (connection, security, network QoS)
- R12: User is optionally able to immerse (if available), and should be in Mixed Reality by default
- R13: User is able to interact with devices via XR UX (devices self describe their interfaces)
- R14: User is able to interact with data via XR UX (devices self describe data dashboards/viewers)
- R15: Hyperlocal Context Engine cannot be shut down by any single company, deprecated, or bought/sold.
- R16: User is able to introspect all data collected by the Context Engine
- R17: No data can leak out to 3rd party servers unless approved by the user.
- R18: User data is portable to other Hyperlocal Context Engine service providers
- R19: The Hyperlocal Context Engine provides a namespace to provide an area for devices and users to interact with services, data, people, and state.
- R20: The namespaces should be easily memorized and short enough to be quickly used to join a namespace
- R21: Namespaces are owned by an entity (a person, a device, an organization)
- R22: Any entity can join a public namespace
- R23: private namespaces will be secured by PKI to allow access
- R24: IoT devices can join namespaces and can register with a shared secret
- R25: WebXR UX for device introspection will be defined in the UI Spec V1
- R26: WebXR UX for people introspection will be defined in the UI Spec V1
- R27: WebXR UX for service introspection will be defined in the UI Spec V1
- R28: WebXR UX for data dashboard introspection will be defined in the UI Spec V1
- R29: Shared experiences are possible in WebXR
- R30: Devices must define their own security (outside of the scope of this PoC)
- R31: Devices must define their capabilities to allow simultaneous control
- R32: Device comissioning is left to be an implementation detail, but assumed devices will be able to handle comissioning automatically if allowed to do so
- R33: At least 2 HMD/Mixed Reality Glasses are supported by the PoC

### Proposal A - locally hosted server behind proxy for hyperlocal context

We will build the architecture pictured below, with a a secure server used as namespace data federation for hyperlocal context, meaning the HMD user can explore this registry on a hyperlocal scale, or any registry compliant with the apis. 

- The hyperlocal context engine is hosted on the web, self hosted
- The context is secured via HTTPS:// and WSS://using a self obtained valid SSL certificate
- The HMD/Mixed Reality Glasses will hit a known url (we will use hlxr.org ... hyperlocal XR) and obtain a temporary or permanent token and namespace


### Proposal B - SaaS hosted server for hyperlocal context

This is the same as proposal A, but it would be  SaaS oriented service provided by a 3rd party company (could be free or not).  This violates the self hosting requirement, but provides all of teh other requirements.

### Alternative 1 - Native XR design for hyperlocal context

A pure native approach that explores the proposed solution using native APIs instead of Web Standandard WebXR.  Surely this will work, but it will break a primary requirement to be portable across platforms.  It can be said that Unity or Unreal engine might provide a way to get native XR across platforms.  But it breaks a second requirement if we require a closed source platform.  And it breaks the third requirement, if it isn't royalty free.  

## Language Considerations / Localization

This is a huge topic, and a pure English centric approach creates problems down the road.  This PoC will focus on a reference implementation available in English and Japanese languages.  The author is a native English speaker.

## Accessibility Considerations

Accessibility is extremely important and a challenge and an opportunity.  We consider WebXR as a mechanism to improve lives, however, more research is needed in this area.  This proposal won't address accessbility considersations and collaborators are desired.

## Prior Work

- Content Centric Networking : https://en.wikipedia.org/wiki/Content_centric_networking

## POC

- A working example will be available for demonstration at this URL: TBD
- Open Source Software: Available Here TBD

### UI Spec V1

TODO

## Recommended Solution

Proposal A.  The other solutions have significant compromises.  The goal is to get away from native applications that are controlled by FANG gatekeepers. 

## Known Limitiations

- WebXR requires a secure context.  There is no "exception" mechanism to ignore HTTPS requirements to connect into XR sessions.  The authors opinion is this is like saying we do not trust the voters.  The user cannot make choices about exceptions.
- WebXR doesn't support *any* URIs besides https://.  Sadly, this blocks a lot of good use cases where any device could expose its own UX and server directly.  The exception to this rule is "localhost", which is possible, but impossible in an HMD or Mixed Reality Glasses and is really designed for desktop developers.
- The limitation makes one wonder if this was designed to force the majority of viable scenarios (including offline use) to require native applications.
