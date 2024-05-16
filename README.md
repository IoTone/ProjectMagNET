# Project MagNET 

An Open Source data sync app and distributed server for iot data.  Intended to provide a place to primarily capture data.  A follow on release will provide a new Open version of IoToneKit, simply letting the user or companies define UX for IoT devices via profile "kits". 

This goal is to make it possible to avoid using proprietary clouds or services, and let the user control how IoT data is used, and control the security. An open server stack will ensure an easy way to capture data locally or in the cloud.

## Documentation 

Please check the [Documentation](https://projectmagnet.github.io) for more details.

## Getting Started

### Magnet App

Review the readme in the subdir for magnet_app.  This requires all of the flutter SDK tools and either ios or android native SDK tools to be installed.

### datasink-proto

Datasink is a concept for a place to store all of your data on a subnet.  It can offer ring buffer of a finite size, or "unlimited" storage.  Ideally this data is persistent and can be guaranteed to be fetched at a future time.  In terms of security, we want to introduce a method of trusting a datasink, and also allow for use of untrusted datasinks.  It's up to the client node to decide if it cares about security.  By default security will be on.  By secure this should be secure transport, verified session key exchange if it is session oriented.  

The real purpose of this first "proto" effort is to create a simple framework for getting some work done in C++.  To this end, we employ a working repository that allows us to build a fully functional C++ project that is non-trivial, as an "expressjs"-like framework.  This will be usable later for more concrete work.  

The build is based on conan/cmake.  However, you won't need to run conan explicitly, you will just need to have it installed.  To install conan, use the recommended practices explained on the conan website.  Noting: installing conan via apt-get or snapd is not the recommended approach.  On mac, use brew to install conan.  Noting, there is a conan2 and a conan1, choose version 2.x.

This building block is for making a restful service.  We can consider adding an MQTT service after this first cut experiment, perhaps in proto2.

#### Notes

This was created using the following command: 
```
git subtree add --prefix datasink-proto git@github.com:truedat101/expresscpp.git master --squash
```

To edit from upstream: 
```
git subtree pull --prefix datasink-proto git@github.com:truedat101/expresscpp.git master --squash 
```

We don't expect to push upstream so no instructions for that.

## License 