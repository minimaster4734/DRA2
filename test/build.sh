#!/bin/sh
U=/usr/local/lib/python3.13/site-packages/unicorn
g++ -O1 -g -std=c++17 -Wno-unused -Wno-invalid-offsetof -I$U/include -o jittest jittest.cpp ../core/jit.cpp ../core/jit_rt.cpp ../core/cpu.cpp ../core/machine.cpp ../core/hw.cpp ../core/gx.cpp ../core/gl.cpp ../core/ax.cpp $U/lib/libunicorn.so.2 -Wl,-rpath,$U/lib -lSDL2 -lGLESv2 -lpthread
