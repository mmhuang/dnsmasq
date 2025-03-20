=背景
- 增加A记录及CNAME记录的redis日志缓存，用于rdns检索及netflow分析中，根据IP打dns及应用标签。
- conf：
```
正向解析redis缓存记录的老化时间
redis_cache_dsn_ttl:

反向解析redis缓存记录的老化时间
redis_cache_ptr_ttl:

redis的服务器配置：
redis_url:
       Redis Standalone
       redis://[:password@]host[:port][/database][?[timeout=timeout[d|h|m|s|ms|us|ns]][&database=database]]
暂不支持下面的形式：
Redis Standalone(SSL)
rediss://[:password@]host[:port][/database][?[timeout=timeout[d|h|m|s|ms|us|ns]][&database=database]]
Redis Standalone(UnixDomainSockets)
redis-socket://path[?[timeout=timeout[d|h|m|s|ms|us|ns]][&database=database]]
Redis Sentinel
redis-sentinel://[:password@]host1[:port1][,host2[:port2]][,hostN[:portN]][/database][?[timeout=timeout[d|h|m|s|ms|us|ns]][&sentinelMasterId=sentinelMasterId][&database=database]]


```
- 数据设计:
```
原则是主要考虑应用逻辑的复杂度及系统的持续运作的复杂度，次要考虑高频操作的效率，最后考虑单机的效率及低频操作的效率。

正向域名解析使用集合：

双表名轮流使用：dns1:www.example.com  
       dns2:www.example.com

当应用读的时候参考当前的时间 是 int(time()/3600*24)%2 奇数还是偶数，选择不同的键名
当应用写入的时候，如果int(time()%(3600*24))>3600*23就写两个键名。也就是重复写键名；其他时间都是只是写入当前的键名。
dns1:www.example.com的有效时间设计，自动回收，如果写入集合成功数量与写入数量相同，就计算并且设计expire时间。目的是避免没有释放旧的记录

键名对应的值，就只是简单记录集合(ip1,ip2,ip3...)



反向域名解析机制：

类似正向解析，使用集合，采用双键名的轮换的机制，轮流老化。只是可能老化时间的设计会更长一点。
ptr1:$ip
ptr2:$ip

键名对应的值，就只是简单记录集合[www.example1.com www.example2.com www.example3.com ]


其他另外使用hash表，另外记录hash的信息(当前需求并不强烈，可以根据需要二期再实现)：
表名：cname:example.com
chain.us=www.example.com → cdn.example.com → us.origin.example.com
chain.sg=www.example.com → cdn.example.com → sg.origin.example.com
ip.us=1.1.1.1,1.1.1.2
ip.sg=1.1.1.1,1.1.1.3

一般查询：正向查询的时候使用hkeys指令。 高频
分析查询：先获取所有的keys再获取内容。
更新与写入：设置键值 更新expire时间。
维护：每个键值写入的时候都配置expire参数。自动回收


======

```

Redis Cache Configuration
-----------------------

The following options configure Redis caching for DNS records:

--redis-url=<url>
    Configure Redis server connection URL. Format:
    redis://[:password@]host[:port][/database][?[timeout=timeout[d|h|m|s|ms|us|ns]][&database=database]]

--redis-cache-dsn-ttl=<seconds>
    Set Time-To-Live in seconds for forward DNS (domain name to IP) Redis cache entries.
    Default is 0 (disabled).

--redis-cache-ptr-ttl=<seconds>  
    Set Time-To-Live in seconds for reverse DNS (IP to domain name) Redis cache entries.
    Default is 0 (disabled).

===
任务：请编程增加上面的参数字段和帮助。用于后续 解析记录写入redis

#file:cache.c #file:forward.c #file:option.c #file:README.md
