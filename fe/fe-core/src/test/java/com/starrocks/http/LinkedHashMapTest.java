package com.starrocks.http;

import org.junit.Test;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.UUID;

import static org.junit.Assert.assertTrue;

public class LinkedHashMapTest {

    @Test
    public void testRemoveEldestEntry() {
        final Map<String, Long> txnNodeMap = new LinkedHashMap<>(512, 0.75f, true) {
            @Override
            protected boolean removeEldestEntry(Map.Entry<String, Long> eldest) {
                // txn最多存储计算(节点 * 512)个label
                return size() > 3 * 512;
            }
        };
         int i = 0;
         while (i < 10000000) {
             String babel = "flink-" + UUID.randomUUID();
             txnNodeMap.put(babel, 1L);
             assertTrue(txnNodeMap.size() <= 3 * 512);
             i++;
         }
        System.out.println(txnNodeMap.size());
    }

}
