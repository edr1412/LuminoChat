# mychat

This mac branch is client-only.
To build, install lumino(mac branch) first.

```sh
brew install boost protobuf
#clone lumino repo, switch to mac branch
mkdir build && cd build && cmake .. && make install
```

Now build this client:

```sh
mkdir build && cd build && cmake .. && make chat_client
```

Run client like this:

```sh
bin/chat_client 172.16.61.131 1145 
```
