// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/catalog/Partition.java

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package com.starrocks.catalog;

import com.google.common.base.Objects;
import com.google.common.base.Preconditions;
import com.google.common.collect.Lists;
import com.google.common.collect.Maps;
import com.google.gson.annotations.SerializedName;
import com.starrocks.catalog.DistributionInfo.DistributionInfoType;
import com.starrocks.catalog.MaterializedIndex.IndexExtState;
import com.starrocks.catalog.MaterializedIndex.IndexState;
import com.starrocks.common.FeConstants;
import com.starrocks.common.io.Text;
import com.starrocks.common.io.Writable;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.DataInput;
import java.io.DataOutput;
import java.io.IOException;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.Map.Entry;
import java.util.stream.Collectors;

/**
 * Internal representation of partition-related metadata.
 */
public class Partition extends MetaObject implements PhysicalPartition, Writable {
    private static final Logger LOG = LogManager.getLogger(Partition.class);

    public static final long PARTITION_INIT_VERSION = 1L;

    public enum PartitionState {
        NORMAL,
        @Deprecated
        ROLLUP,
        @Deprecated
        SCHEMA_CHANGE
    }

    @SerializedName(value = "id")
    private long id;

    private long beforeRestoreId;

    @SerializedName(value = "name")
    private String name;
    @SerializedName(value = "state")
    private PartitionState state;
    @SerializedName(value = "idToSubPartition")
    private Map<Long, PhysicalPartitionImpl> idToSubPartition = Maps.newHashMap();

    @SerializedName(value = "distributionInfo")
    private DistributionInfo distributionInfo;

    @SerializedName(value = "shardGroupId")
    private long shardGroupId;

    /* Physical Partition Member */
    @SerializedName(value = "isImmutable")
    private boolean isImmutable = false;

    @SerializedName(value = "baseIndex")
    private MaterializedIndex baseIndex;
    /**
     * Visible rollup indexes are indexes which are visible to user.
     * User can do query on them, show them in related 'show' stmt.
     */
    @SerializedName(value = "idToVisibleRollupIndex")
    private Map<Long, MaterializedIndex> idToVisibleRollupIndex = Maps.newHashMap();
    /**
     * Shadow indexes are indexes which are not visible to user.
     * Query will not run on these shadow indexes, and user can not see them neither.
     * But load process will load data into these shadow indexes.
     */
    @SerializedName(value = "idToShadowIndex")
    private Map<Long, MaterializedIndex> idToShadowIndex = Maps.newHashMap();

    /**
     * committed version(hash): after txn is committed, set committed version(hash)
     * visible version(hash): after txn is published, set visible version
     * next version(hash): next version is set after finished committing, it should equal to committed version + 1
     */

    // not have committedVersion because committedVersion = nextVersion - 1
    @SerializedName(value = "visibleVersion")
    private long visibleVersion;
    @SerializedName(value = "visibleVersionTime")
    private long visibleVersionTime;
    /**
     * ID of the transaction that has committed current visible version.
     * Just for tracing the txn log, no need to persist.
     */
    private long visibleTxnId = -1;
    @SerializedName(value = "nextVersion")
    private long nextVersion;

    private volatile long lastVacuumTime = 0;

    private volatile long minRetainVersion = 0;

    private Partition() {
    }

    public Partition(long id, String name,
                     MaterializedIndex baseIndex,
                     DistributionInfo distributionInfo) {
        this.id = id;
        this.name = name;
        this.state = PartitionState.NORMAL;

        this.baseIndex = baseIndex;

        this.visibleVersion = PARTITION_INIT_VERSION;
        this.visibleVersionTime = System.currentTimeMillis();
        // PARTITION_INIT_VERSION == 1, so the first load version is 2 !!!
        this.nextVersion = PARTITION_INIT_VERSION + 1;

        this.distributionInfo = distributionInfo;
    }

    public Partition(long id, String name,
                     MaterializedIndex baseIndex,
                     DistributionInfo distributionInfo, long shardGroupId) {
        this(id, name, baseIndex, distributionInfo);
        this.shardGroupId = shardGroupId;
    }

    public Partition shallowCopy() {
        Partition partition = new Partition();
        partition.id = this.id;
        partition.name = this.name;
        partition.state = this.state;
        partition.baseIndex = this.baseIndex;
        partition.idToVisibleRollupIndex = Maps.newHashMap(this.idToVisibleRollupIndex);
        partition.idToShadowIndex = Maps.newHashMap(this.idToShadowIndex);
        partition.visibleVersion = this.visibleVersion;
        partition.visibleVersionTime = this.visibleVersionTime;
        partition.nextVersion = this.nextVersion;
        partition.distributionInfo = this.distributionInfo;
        partition.shardGroupId = this.shardGroupId;
        partition.idToSubPartition = Maps.newHashMap(this.idToSubPartition);
        return partition;
    }

    @Override
    public void setIdForRestore(long id) {
        this.beforeRestoreId = this.id;
        this.id = id;
    }

    public long getId() {
        return this.id;
    }

    @Override
    public long getBeforeRestoreId() {
        return beforeRestoreId;
    }

    @Override
    public void setImmutable(boolean isImmutable) {
        this.isImmutable = isImmutable;
    }

    public boolean isImmutable() {
        return this.isImmutable;
    }

    public void addSubPartition(PhysicalPartition subPartition) {
        if (subPartition instanceof PhysicalPartitionImpl) {
            idToSubPartition.put(subPartition.getId(), (PhysicalPartitionImpl) subPartition);
        }
    }

    public void removeSubPartition(long id) {
        idToSubPartition.remove(id);
    }

    public Collection<PhysicalPartition> getSubPartitions() {
        List<PhysicalPartition> subPartitions = idToSubPartition.values().stream().collect(Collectors.toList());
        subPartitions.add(this);
        return subPartitions;
    }

    public PhysicalPartition getSubPartition(long id) {
        return this.id == id ? this : idToSubPartition.get(id);
    }

    public long getParentId() {
        return this.id;
    }

    public void setParentId(long parentId) {
        return;
    }

    public long getShardGroupId() {
        return this.shardGroupId;
    }

    public void setName(String newName) {
        this.name = newName;
    }

    public String getName() {
        return this.name;
    }

    public void setState(PartitionState state) {
        this.state = state;
    }

    /*
     * If a partition is overwritten by a restore job, we need to reset all version info to
     * the restored partition version info)
     */
    public void updateVersionForRestore(long visibleVersion) {
        this.setVisibleVersion(visibleVersion);
        this.nextVersion = this.visibleVersion + 1;
        LOG.info("update partition {} version for restore: visible: {}, next: {}",
                name, visibleVersion, nextVersion);
    }

    public void updateVisibleVersion(long visibleVersion) {
        updateVisibleVersion(visibleVersion, System.currentTimeMillis());
    }

    public void updateVisibleVersion(long visibleVersion, long visibleVersionTime) {
        this.setVisibleVersion(visibleVersion, visibleVersionTime);
    }

    public void updateVisibleVersion(long visibleVersion, long visibleVersionTime, long visibleTxnId) {
        setVisibleVersion(visibleVersion, visibleVersionTime, visibleTxnId);
    }

    public long getVisibleVersion() {
        return visibleVersion;
    }

    public long getVisibleVersionTime() {
        return visibleVersionTime;
    }

    // The method updateVisibleVersion is called when fe restart, the visibleVersionTime is updated
    private void setVisibleVersion(long visibleVersion) {
        this.visibleVersion = visibleVersion;
        this.visibleVersionTime = System.currentTimeMillis();
    }

    public void setVisibleVersion(long visibleVersion, long visibleVersionTime) {
        this.visibleVersion = visibleVersion;
        this.visibleVersionTime = visibleVersionTime;
    }

    public void setVisibleVersion(long visibleVersion, long visibleVersionTime, long visibleTxnId) {
        this.visibleVersion = visibleVersion;
        this.visibleVersionTime = visibleVersionTime;
        this.visibleTxnId = visibleTxnId;
    }

    public long getVisibleTxnId() {
        return visibleTxnId;
    }

    public PartitionState getState() {
        return this.state;
    }

    public DistributionInfo getDistributionInfo() {
        return distributionInfo;
    }

    public void createRollupIndex(MaterializedIndex mIndex) {
        if (mIndex.getState().isVisible()) {
            this.idToVisibleRollupIndex.put(mIndex.getId(), mIndex);
        } else {
            this.idToShadowIndex.put(mIndex.getId(), mIndex);
        }
    }

    public MaterializedIndex deleteRollupIndex(long indexId) {
        if (this.idToVisibleRollupIndex.containsKey(indexId)) {
            return idToVisibleRollupIndex.remove(indexId);
        } else {
            return idToShadowIndex.remove(indexId);
        }
    }

    public void setBaseIndex(MaterializedIndex baseIndex) {
        this.baseIndex = baseIndex;
    }

    public MaterializedIndex getBaseIndex() {
        return baseIndex;
    }

    public long getNextVersion() {
        return nextVersion;
    }

    public void setNextVersion(long nextVersion) {
        this.nextVersion = nextVersion;
    }

    public long getCommittedVersion() {
        return this.nextVersion - 1;
    }

    public MaterializedIndex getIndex(long indexId) {
        if (baseIndex.getId() == indexId) {
            return baseIndex;
        }
        if (idToVisibleRollupIndex.containsKey(indexId)) {
            return idToVisibleRollupIndex.get(indexId);
        } else {
            return idToShadowIndex.get(indexId);
        }
    }

    public List<MaterializedIndex> getMaterializedIndices(IndexExtState extState) {
        List<MaterializedIndex> indices = Lists.newArrayList();
        switch (extState) {
            case ALL:
                indices.add(baseIndex);
                indices.addAll(idToVisibleRollupIndex.values());
                indices.addAll(idToShadowIndex.values());
                break;
            case VISIBLE:
                indices.add(baseIndex);
                indices.addAll(idToVisibleRollupIndex.values());
                break;
            case SHADOW:
                indices.addAll(idToShadowIndex.values());
            default:
                break;
        }
        return indices;
    }

    public int getMaterializedIndicesCount(IndexExtState extState) {
        switch (extState) {
            case ALL:
                return 1 + idToVisibleRollupIndex.size() + idToShadowIndex.size();
            case VISIBLE:
                return 1 + idToVisibleRollupIndex.size();
            case SHADOW:
                return idToVisibleRollupIndex.size();
            default:
                return 0;
        }
    }

    public int getVisibleMaterializedIndicesCount() {
        int count = 0;
        for (PhysicalPartition subPartition : getSubPartitions()) {
            count += subPartition.getMaterializedIndices(IndexExtState.VISIBLE).size();
        }
        return count;
    }

    public long storageDataSize() {
        long dataSize = 0;
        for (MaterializedIndex mIndex : getMaterializedIndices(IndexExtState.VISIBLE)) {
            dataSize += mIndex.getDataSize();
        }
        return dataSize;
    }

    public long getDataSize() {
        long dataSize = 0;
        for (PhysicalPartition subPartition : getSubPartitions()) {
            dataSize += subPartition.storageDataSize();
        }
        return dataSize;
    }

    public long storageRowCount() {
        long rowCount = 0;
        for (MaterializedIndex mIndex : getMaterializedIndices(IndexExtState.VISIBLE)) {
            rowCount += mIndex.getRowCount();
        }
        return rowCount;
    }

    public long getRowCount() {
        long rowCount = 0;
        for (PhysicalPartition subPartition : getSubPartitions()) {
            rowCount += subPartition.storageRowCount();
        }
        return rowCount;
    }

    public long storageReplicaCount() {
        long replicaCount = 0;
        for (MaterializedIndex mIndex : getMaterializedIndices(IndexExtState.VISIBLE)) {
            replicaCount += mIndex.getReplicaCount();
        }
        return replicaCount;
    }

    public long getReplicaCount() {
        long replicaCount = 0;
        for (PhysicalPartition subPartition : getSubPartitions()) {
            replicaCount += subPartition.storageReplicaCount();
        }
        return replicaCount;
    }

    public boolean hasMaterializedView() {
        return !idToVisibleRollupIndex.isEmpty();
    }

    public boolean hasStorageData() {
        // The fe unit test need to check the selected index id without any data.
        // So if set FeConstants.runningUnitTest, we can ensure that the number of partitions is not empty,
        // And the test case can continue to execute the logic of 'select best roll up'
        return ((visibleVersion != PARTITION_INIT_VERSION)
                || FeConstants.runningUnitTest);
    }

    public boolean hasData() {
        boolean hasData = false;
        for (PhysicalPartition subPartition : getSubPartitions()) {
            hasData |= subPartition.hasStorageData();
        }
        return hasData;
    }

    public boolean isFirstLoad() {
        return visibleVersion == PARTITION_INIT_VERSION + 1;
    }

    /*
     * Change the index' state from SHADOW to NORMAL
     * Also move it to idToVisibleRollupIndex if it is not the base index.
     */
    public boolean visualiseShadowIndex(long shadowIndexId, boolean isBaseIndex) {
        MaterializedIndex shadowIdx = idToShadowIndex.remove(shadowIndexId);
        if (shadowIdx == null) {
            return false;
        }
        Preconditions.checkState(!idToVisibleRollupIndex.containsKey(shadowIndexId), shadowIndexId);
        shadowIdx.setState(IndexState.NORMAL);
        if (isBaseIndex) {
            baseIndex = shadowIdx;
        } else {
            idToVisibleRollupIndex.put(shadowIndexId, shadowIdx);
        }
        LOG.info("visualise the shadow index: {}", shadowIndexId);
        return true;
    }

    public static Partition read(DataInput in) throws IOException {
        Partition partition = new Partition();
        partition.readFields(in);
        return partition;
    }

    @Override
    public void write(DataOutput out) throws IOException {
        super.write(out);

        out.writeLong(id);
        Text.writeString(out, name);
        Text.writeString(out, state.name());

        baseIndex.write(out);

        int rollupCount = (idToVisibleRollupIndex != null) ? idToVisibleRollupIndex.size() : 0;
        out.writeInt(rollupCount);
        if (idToVisibleRollupIndex != null) {
            for (Map.Entry<Long, MaterializedIndex> entry : idToVisibleRollupIndex.entrySet()) {
                entry.getValue().write(out);
            }
        }

        out.writeInt(idToShadowIndex.size());
        for (MaterializedIndex shadowIndex : idToShadowIndex.values()) {
            shadowIndex.write(out);
        }

        out.writeLong(visibleVersion);
        out.writeLong(visibleVersionTime);
        out.writeLong(0); // write a version_hash for compatibility

        out.writeLong(nextVersion);
        out.writeLong(0); // write a version_hash for compatibility
        out.writeLong(0); // write a version_hash for compatibility

        Text.writeString(out, distributionInfo.getType().name());
        distributionInfo.write(out);
    }

    @Override
    public void readFields(DataInput in) throws IOException {
        super.readFields(in);

        id = in.readLong();
        name = Text.readString(in);
        state = PartitionState.valueOf(Text.readString(in));

        baseIndex = MaterializedIndex.read(in);

        int rollupCount = in.readInt();
        for (int i = 0; i < rollupCount; ++i) {
            MaterializedIndex rollupTable = MaterializedIndex.read(in);
            idToVisibleRollupIndex.put(rollupTable.getId(), rollupTable);
        }

        int shadowIndexCount = in.readInt();
        for (int i = 0; i < shadowIndexCount; i++) {
            MaterializedIndex shadowIndex = MaterializedIndex.read(in);
            idToShadowIndex.put(shadowIndex.getId(), shadowIndex);
        }

        visibleVersion = in.readLong();
        visibleVersionTime = in.readLong();
        in.readLong(); // read a version_hash for compatibility
        nextVersion = in.readLong();
        in.readLong(); // read a version_hash for compatibility
        in.readLong(); // read a version_hash for compatibility
        DistributionInfoType distriType = DistributionInfoType.valueOf(Text.readString(in));
        if (distriType == DistributionInfoType.HASH) {
            distributionInfo = HashDistributionInfo.read(in);
        } else if (distriType == DistributionInfoType.RANDOM) {
            distributionInfo = RandomDistributionInfo.read(in);
        } else {
            throw new IOException("invalid distribution type: " + distriType);
        }
    }

    @Override
    public int hashCode() {
        return Objects.hashCode(visibleVersion, baseIndex, distributionInfo);
    }

    @Override
    public boolean equals(Object obj) {
        if (this == obj) {
            return true;
        }
        if (!(obj instanceof Partition)) {
            return false;
        }

        Partition partition = (Partition) obj;
        if (idToVisibleRollupIndex != partition.idToVisibleRollupIndex) {
            if (idToVisibleRollupIndex.size() != partition.idToVisibleRollupIndex.size()) {
                return false;
            }
            for (Entry<Long, MaterializedIndex> entry : idToVisibleRollupIndex.entrySet()) {
                long key = entry.getKey();
                if (!partition.idToVisibleRollupIndex.containsKey(key)) {
                    return false;
                }
                if (!entry.getValue().equals(partition.idToVisibleRollupIndex.get(key))) {
                    return false;
                }
            }
        }

        return (visibleVersion == partition.visibleVersion)
                && (baseIndex.equals(partition.baseIndex)
                && distributionInfo.equals(partition.distributionInfo));
    }

    @Override
    public String toString() {
        StringBuilder buffer = new StringBuilder();
        buffer.append("partition_id: ").append(id).append("; ");
        buffer.append("name: ").append(name).append("; ");
        buffer.append("partition_state.name: ").append(state.name()).append("; ");

        buffer.append("base_index: ").append(baseIndex.toString()).append("; ");

        int rollupCount = (idToVisibleRollupIndex != null) ? idToVisibleRollupIndex.size() : 0;
        buffer.append("rollup count: ").append(rollupCount).append("; ");

        if (idToVisibleRollupIndex != null) {
            for (Map.Entry<Long, MaterializedIndex> entry : idToVisibleRollupIndex.entrySet()) {
                buffer.append("rollup_index: ").append(entry.getValue().toString()).append("; ");
            }
        }

        buffer.append("committedVersion: ").append(visibleVersion).append("; ");
        buffer.append("committedVersionHash: ").append(0).append("; ");

        buffer.append("distribution_info.type: ").append(distributionInfo.getType().name()).append("; ");
        buffer.append("distribution_info: ").append(distributionInfo.toString());

        return buffer.toString();
    }

    public boolean convertRandomDistributionToHashDistribution(List<Column> baseSchema) {
        boolean hasChanged = false;
        if (distributionInfo.getType() == DistributionInfoType.RANDOM) {
            distributionInfo = ((RandomDistributionInfo) distributionInfo).toHashDistributionInfo(baseSchema);
            hasChanged = true;
        }
        return hasChanged;
    }

    public long getLastVacuumTime() {
        return lastVacuumTime;
    }

    public void setLastVacuumTime(long lastVacuumTime) {
        this.lastVacuumTime = lastVacuumTime;
    }

    public long getMinRetainVersion() {
        return minRetainVersion;
    }

    public void setMinRetainVersion(long minRetainVersion) {
        this.minRetainVersion = minRetainVersion;
    }
}
