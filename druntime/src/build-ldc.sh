#!/usr/bin/env bash

OLDHOME=$HOME
export HOME=`pwd`

goerror(){
    export HOME=$OLDHOME
    echo "="
    echo "= *** Error ***"
    echo "="
    exit 1
}

make clean -fldc-posix.mak          || goerror
make -fldc-posix.mak                || goerror
chmod 644 ../import/core/*.di       || goerror
chmod 644 ../import/core/sync/*.di  || goerror

export HOME=$OLDHOME
