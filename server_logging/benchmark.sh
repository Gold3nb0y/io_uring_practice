#!/bin/bash
#
NUM=0x100000000
PORT=9001

python3 -c "print('A'*$NUM)" | nc -N localhost 9001
