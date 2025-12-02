package com.starrocks.http.rest;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.UUID;

public class LinkedHashMapTest {

    final static Map<String, Long> txnNodeMap = new LinkedHashMap<>(512, 0.75f, true) {
        @Override
        protected boolean removeEldestEntry(Map.Entry<String, Long> eldest) {
            // txn最多存储计算(节点 * 512)个label
            return size() > 3 * 512;
        }
    };

    public static void main(String[] args) throws Exception{
        int i = 0;
        while (i < 10000000) {
            String babel = "flink-" + UUID.randomUUID();
            txnNodeMap.put(babel, 1L);
            if(txnNodeMap.size() > 3 * 512){
                System.out.println("error");
            }
            i++;
            Thread.sleep(1000);
        }
        System.out.println(txnNodeMap.size());
    }


}
