#ifndef GEN_REF_IMAGE_HPP
#define GEN_REF_IMAGE_HPP

// The reference's address space for the translated functions: its data sections mapped at their
// original addresses from the clean executable at `path`, and jumps at the reference addresses
// of every function the port can run (see RefImage.cpp). Call before anything translated runs.
bool RefImageInitialize(const char* path);
bool RefImageMapped();

#endif
