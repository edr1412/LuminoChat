# mychat

## Dependencies
# sudo apt install libhiredis-dev # don't do this, because hiredis-cluster automatically install hiredis and they conflict
make sure your muduo has protobuf feature, otherwise install libprotobuf-dev before you build muduo
sudo apt install pkg-config libglib2.0-dev libevent-dev
git clone https://github.com/Nordix/hiredis-cluster.git && cd hiredis-cluster && mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_SSL=OFF .. && make && sudo make install

## Deploy
sudo apt install redis-server
1. not using cluster
    sudo vim /etc/redis/redis.conf # uncomment bind, protected-mode = no
    sudo systemctl start redis-server
    uncomment the `#define USE_REDIS_CLUSTER` in `server.cc`
2. using cluster
    say we have 3 machines, 172.16.61.131, 172.16.61.133, 172.16.61.134
    master nodes: 172.16.61.131:6379, 172.16.61.133:6379, 172.16.61.134:6379
    slave nodes: 172.16.61.133:6380, 172.16.61.134:6380, 172.16.61.131:6380
    do the following on each machine:
    sudo vim /etc/redis/redis-6379.conf
    sudo vim /etc/redis/redis-6380.conf
    ```
    # /etc/redis/redis-6379.conf
    port 6379
    cluster-enabled yes
    cluster-config-file nodes-6379.conf
    cluster-node-timeout 5000
    appendonly yes

    # /etc/redis/redis-6380.conf
    port 6380
    cluster-enabled yes
    cluster-config-file nodes-6380.conf
    cluster-node-timeout 5000
    appendonly yes
    ```
    redis-server /etc/redis/redis-6379.conf
    redis-server /etc/redis/redis-6380.conf

    then, do the following command once, and hit yes:
    redis-cli --cluster create 172.16.61.131:6379 172.16.61.133:6379 172.16.61.134:6379 172.16.61.133:6380 172.16.61.134:6380 172.16.61.131:6380 --cluster-replicas 1
    check the cluster status:
    redis-cli -c -h 172.16.61.131 -p 6379 cluster nodes

    we run chat server on each machine, and they will connect to the cluster automatically.
    then config the nginx to load balance the chat server. say we use 172.16.61.133 (macOS) as the load balancer,
    sudo vim /usr/local/etc/nginx/nginx.conf # add the following before http
    ```
    stream {
        upstream backend {
            server 172.16.61.131:1145;
            server 172.16.61.133:1145;
            server 172.16.61.134:1145;
        }

        server {
            listen 1145;
            proxy_pass backend;
        }
    }
    ```
    brew services reload nginx

    now we can connect client to 172.16.61.1:1145



    