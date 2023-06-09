// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

package com.starrocks.catalog;

import com.google.common.collect.Lists;
import com.google.gson.annotations.SerializedName;
import com.starrocks.common.io.Text;
import com.starrocks.common.io.Writable;
import com.starrocks.common.proc.BaseProcResult;
import com.starrocks.persist.gson.GsonUtils;

import java.io.DataInput;
import java.io.DataOutput;
import java.io.IOException;
import java.util.Map;

public class Catalog implements Writable {
    public static final String CATALOG_TYPE = "type";

    // Reserved fields for later support operations such as rename
    @SerializedName("id")
    protected long id;
    @SerializedName(value = "name")
    protected String name;
    @SerializedName(value = "comment")
    protected String comment;
    @SerializedName(value = "config")
    protected Map<String, String> config;

    public Catalog(long id, String name, Map<String, String> config, String comment) {
        this.id = id;
        this.name = name;
        this.config = config;
        this.comment = comment;
    }

    public long getId() {
        return id;
    }

    public String getName() {
        return name;
    }

    public String getType() {
        return config.get(CATALOG_TYPE);
    }

    public Map<String, String> getConfig() {
        return config;
    }

    public String getComment() {
        return comment;
    }

    public String getDisplayComment() {
        return CatalogUtils.addEscapeCharacter(comment);
    }

    public void getProcNodeData(BaseProcResult result) {
        result.addRow(Lists.newArrayList(this.getName(), config.get(CATALOG_TYPE), this.getComment()));
    }

    public static Catalog read(DataInput in) throws IOException {
        String json = Text.readString(in);
        return GsonUtils.GSON.fromJson(json, Catalog.class);
    }

    @Override
    public void write(DataOutput out) throws IOException {
        String json = GsonUtils.GSON.toJson(this);
        Text.writeString(out, json);
    }
}
