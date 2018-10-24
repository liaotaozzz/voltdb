/* This file is part of VoltDB.
 * Copyright (C) 2008-2018 VoltDB Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */

package org.voltdb.export;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.Map;

import org.voltcore.logging.VoltLogger;
import org.voltcore.utils.CoreUtils;
import org.voltdb.VoltDB;
import org.voltdb.VoltDBInterface;

import com.google_voltpatches.common.util.concurrent.ListeningExecutorService;

/**
 * @author rdykiel
 *
 * Export Data Source Executor Factory.
 *
 * Singleton instance allocating {@link ListeningExecutorService} for {@link ExportDataSource}.
 *
 * Each {@code ExportDataSource} instance uses a mono-thread {@code ListeningExecutorService},
 * so that executing its runnables in that unique thread ensures that the instance is implictly
 * synchronized. However using a different thread per instance results in an explosion of threads
 * (one thread per exported stream multiplied by the number of partitions).
 *
 * The design of a {@code ExportDataSource} relies on all the runnables being executed by the same
 * thread, but nothing prevents sharing the thread among multiple instances: this would still ensure
 * that each instance is touched by a unique thread, while allowing reducing the overall number of
 * threads.
 *
 * This factory implements a thread sharing policy using a fixed number of threads. The default number
 * of threads is 1: all {@code ExportDataSource} instances sharing the same thread. This limit may be
 * increased with the MAX_EXPORT_THREADS system property. Further, the thread sharing policy ensures
 * that all {@code ExportDataSource} instances for a given partition share the same thread.
 *
 * Note on synchronization: we don't need a sophisticated synchronization mechanism here, as the
 * factory is just used by the {@code ExportDataSource} constructors.
 *
 * Finally, we need to maintain a reference count on the executors in order to handle shutdown.
 */
public class ExecutorFactory {


    private static final VoltLogger exportLog = new VoltLogger("EXPORT");
    public static final String MAX_EXPORT_THREADS = "MAX_EXPORT_THREADS";

    private Integer m_maxThreads;
    private int m_nextAlloc;

    private ArrayList<ListeningExecutorService> m_executors;
    private IdentityHashMap<ListeningExecutorService, Integer> m_refCounts;

    private Map<Integer, ListeningExecutorService> m_pid2execMap;

    /**
     * Singleton accessor
     *
     * @return the {@code EDSExecutorFactory } instance
     */
    public static final ExecutorFactory instance() {
        return EXEC_FACTORY_INSTANCE;
    }

    /**
     * Package-private constructor for JUnit tests
     */
    ExecutorFactory() {
    }

    /**
     * @return true if initialized
     */
    private boolean isInitialized() {
        return m_maxThreads != null;
    }

    /**
     * Lazy initialization on first request for an executor
     */
    private void initialize() {

        if (isInitialized()) {
            return;
        }

        // Note - this theoretically could throw on permissions
        m_maxThreads = getConfigMaxThreads();
        int minThreads = getMinThreads();
        if (m_maxThreads < 1) {
            exportLog.warn("Parameter \"" + MAX_EXPORT_THREADS
                    + "\" should have a positive value, forcing to default value of " + minThreads);
            m_maxThreads = minThreads;
        }
        else {
            int localSitesCount = getLocalSitesCount();
            if (localSitesCount == 0) {
                exportLog.warn("Parameter \"" + MAX_EXPORT_THREADS
                        + "\" cannot be checked, forcing to default value of " + minThreads);
                m_maxThreads = minThreads;
            }
            else if (m_maxThreads > localSitesCount) {
                exportLog.warn("Parameter \"" + MAX_EXPORT_THREADS
                        + "\" exceeds local sites count, forcing to " + localSitesCount);
                m_maxThreads = localSitesCount;
            }
        }
        exportLog.info("Export Data Sources running with " + m_maxThreads + " executor threads");

        m_nextAlloc = 0;
        m_executors = new ArrayList<>(m_maxThreads);
        m_refCounts = new IdentityHashMap<>();
        m_pid2execMap = new HashMap<>();
    }

    /**
     * @return the minimal number of threads to use (same as {@link DRProducer}
     */
    int getMinThreads() {
        return Math.max(2, CoreUtils.availableProcessors() / 4);
    }

    /**
     * @return max threads configured, package private for JUnit tests
     */
    Integer getConfigMaxThreads() {
        return Integer.getInteger(MAX_EXPORT_THREADS, getMinThreads());
    }

    /**
     * @return local sites count, or 0 if undefined, package private for JUnit tests
     */
    int getLocalSitesCount() {
        int locSitesCount = 0;
        // JUnit environment may give inconsistent answers
        VoltDBInterface volt = VoltDB.instance();
        if (volt != null &&  volt.getCatalogContext() != null
                && volt.getCatalogContext().getNodeSettings() != null) {
            locSitesCount = volt.getCatalogContext().getNodeSettings().getLocalSitesCount();
        }
        return locSitesCount;
    }

    /**
     * @return the max thread count this factory is currently operating upon, or 0 if uninitialized
     */
    public synchronized int getMaxThreadCount() {
        return m_maxThreads;
    }

    /**
     * @return current
     */
    public synchronized int getCurrentThreadCount() {
        return m_executors.size();
    }

    /**
     * Get an executor for an {@link ExportDataSource} identified by partitionId and tableName
     *
     * @param partitionId
     * @param tableName
     * @return {@link ListeningExecutorService} allocated
     */
    public synchronized ListeningExecutorService getExecutor(int partitionId, String tableName) {

        initialize();
        return allocate(partitionId, tableName);
    }

    /**
     * Free an executor used by an export data source identified by partitionId and tableName
     *
     * @param partitionId
     * @param tableName
     */
    public synchronized void freeExecutor(int partitionId, String tableName) {

        if (!isInitialized()) {
            // Paranoid check for maintenance
            throw new IllegalStateException("Export Data Source for table: " + tableName
            + ", partition: " + partitionId + " frees uninitialized executor");
        }
        release(partitionId, tableName);
    }

    /**
     * Allocate executor for new export data source
     *
     * @param partitionId
     * @param tableName
     * @return
     */
    private ListeningExecutorService allocate(int partitionId, String tableName) {

        ListeningExecutorService les = m_pid2execMap.get(partitionId);
        if (les != null) {
            // Existing executor for this partition
            int refCount = m_refCounts.get(les).intValue() + 1;
            m_refCounts.put(les, refCount);

            trace("Allocated existing executor %d, for partition %d",
                    les.hashCode(), partitionId);
        }
        else if (m_executors.size() < m_maxThreads) {
            // Create new executor for partition
            les = CoreUtils.getListeningExecutorService("ExportDataSource executor", 1);
            m_executors.add(les);
            m_refCounts.put(les, 1);
            m_pid2execMap.put(partitionId, les);

            trace("Allocated new executor %d, for partition %d, %d executors running",
                    les.hashCode(), partitionId, m_executors.size());
        }
        else {
            // Re-use existing executor for this partition
            if (m_nextAlloc >= m_executors.size()) {
                m_nextAlloc = 0;
            }
            les = m_executors.get(m_nextAlloc);
            int refCount = m_refCounts.get(les).intValue() + 1;
            m_refCounts.put(les, refCount);
            m_pid2execMap.put(partitionId, les);

            trace("Allocated executor %d, index %d, for partition %d",
                    les.hashCode(), m_nextAlloc, partitionId);
            m_nextAlloc += 1;
        }
        return les;
    }

    /**
     * Release executor for export data source
     *
     * @param partitionId
     * @param tableName
     */
    private void release(int partitionId, String tableName) {

        // The partition must be mapped...
        ListeningExecutorService les = m_pid2execMap.get(partitionId);
        if (les == null) {
            throw new IllegalStateException("Export Data Source for table: " + tableName
            + ", partition: " + partitionId + " frees unallocated executor");
        }

        int refCount = m_refCounts.get(les).intValue() - 1;
        if (refCount < 0) {
            throw new IllegalStateException("Invalid refCount for table: " + tableName
                        + "partition: " + partitionId);
        }

        if (refCount == 0) {
            m_executors.remove(les);
            m_refCounts.remove(les);

            // The executor may remain mapped for other partitions, remove all mappings
            m_pid2execMap.values().removeIf(v -> v == les);

            if (m_nextAlloc > m_executors.size()) {
                m_nextAlloc = 0;
            }
            les.shutdown();
            trace("Shutdown executor %d", les.hashCode());
        }
        else {
            m_refCounts.put(les, refCount);
            trace("Defer shutdown executor %d, refCount %d", les.hashCode(), refCount);
        }
    }

    private void trace(String format, Object... arguments) {
        if (exportLog.isTraceEnabled()) {
            if (arguments != null && arguments.length > 0) {
                exportLog.trace(String.format(format, arguments));
            } else {
                exportLog.trace(format);
            }
        }
    }

    private static final ExecutorFactory EXEC_FACTORY_INSTANCE = new ExecutorFactory();
}
