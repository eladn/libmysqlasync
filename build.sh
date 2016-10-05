#! /bin/bash

rm -rf build
mkdir build
cd build
cmake ..
if (( $? == 0 )); then
	make
	if (( $? == 0 )); then
		sudo ldconfig
		sudo make install
	fi
fi
