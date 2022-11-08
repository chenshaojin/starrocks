// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/analysis/ColumnDef.java

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

package com.starrocks.analysis;

import com.google.common.base.Preconditions;
import com.google.common.collect.Sets;
import com.starrocks.catalog.AggregateType;
import com.starrocks.catalog.Column;
import com.starrocks.catalog.FunctionSet;
import com.starrocks.catalog.PrimitiveType;
import com.starrocks.catalog.ScalarType;
import com.starrocks.catalog.Type;
import com.starrocks.common.AnalysisException;
import com.starrocks.common.FeNameFormat;

import java.util.ArrayList;
import java.util.Set;

import static com.starrocks.catalog.DefaultExpr.SUPPORTED_DEFAULT_FNS;

// Column definition which is generated by SQL syntax parser
// Syntax:
//      name type [key] [agg_type] [NULL | NOT NULL] [DEFAULT default_value] [comment]
// Example:
//      id bigint key NOT NULL DEFAULT "-1" "user id"
//      pv bigint sum NULL DEFAULT "-1" "page visit"
public class ColumnDef {
    /*
     * User can set default value for a column
     * eg:
     *     k1 INT NOT NULL DEFAULT "10"
     *     k1 INT NULL
     *     k1 INT NULL DEFAULT NULL
     *
     * ColumnDef will be transformed to Column in Analysis phase, and in Column, default value is a String.
     * No matter does the user set the default value as NULL explicitly, or not set default value,
     * the default value in Column will be "null", so that StarRocks can not distinguish between "not set" and "set as null".
     *
     * But this is OK because Column has another attribute "isAllowNull".
     * If the column is not allowed to be null, and user does not set the default value,
     * even if default value saved in Column is null, the "null" value can not be loaded into this column,
     * so data correctness can be guaranteed.
     */
    public static class DefaultValueDef {
        public boolean isSet;
        public Expr expr;

        public DefaultValueDef(boolean isSet, Expr expr) {
            this.isSet = isSet;
            // make expr always not null
            if (expr != null) {
                this.expr = expr;
            } else {
                this.expr = NullLiteral.create(Type.VARCHAR);
            }
        }

        private static final String ZERO = new String(new byte[] {0});
        // no default value
        public static DefaultValueDef NOT_SET = new DefaultValueDef(false, NullLiteral.create(Type.VARCHAR));
        // default null
        public static DefaultValueDef NULL_DEFAULT_VALUE = new DefaultValueDef(true, NullLiteral.create(Type.VARCHAR));
        // default "value", "0" means empty hll or bitmap
        public static DefaultValueDef EMPTY_VALUE = new DefaultValueDef(true, new StringLiteral(ZERO));
        // default value for date type CURRENT_TIMESTAMP
        public static DefaultValueDef CURRENT_TIMESTAMP_VALUE = new DefaultValueDef(true,
                new FunctionCallExpr("now", new ArrayList<>()));
    }

    private final static Set<String> charsetNames;

    static {
        charsetNames = Sets.newHashSet();
        charsetNames.add("utf8");
        charsetNames.add("gbk");
    }

    // parameter initialized in constructor
    private final String name;
    private final TypeDef typeDef;
    private final String DEFAULT_CHARSET = "utf8";
    private String charsetName = DEFAULT_CHARSET;
    private AggregateType aggregateType;
    private boolean isKey;
    // Primary-key column should obey the not-null constraint. When creating a table, the not-null constraint will add to the primary-key column default. If the user specifies NULL explicitly, semantics analysis will report an error.
    // Now, isAllowNull is used to indicate a null constraint hold or not. Primary-key and non-primary-key columns obey different constraints, so the isAllowNull can not be assigned a default value.
    // Add a new variable name isAllowNullImplicit to indicate the message. If isAllowNullImplicit=true, it indicates the null constraint is obeyed implicitly.
    private boolean isAllowNullImplicit = false;
    private Boolean isAllowNull;
    private DefaultValueDef defaultValueDef;
    private final String comment;

    public ColumnDef(String name, TypeDef typeDef) {
        this(name, typeDef, null, false, null, false, DefaultValueDef.NOT_SET, "");
    }

    public ColumnDef(String name, TypeDef typeDef, boolean isKey, AggregateType aggregateType,
                     Boolean isAllowNull, DefaultValueDef defaultValueDef, String comment) {
        this(name, typeDef, null, isKey, aggregateType, isAllowNull, defaultValueDef, comment);
    }

    public ColumnDef(String name, TypeDef typeDef, String charsetName, boolean isKey, AggregateType aggregateType,
                     Boolean isAllowNull, DefaultValueDef defaultValueDef, String comment) {
        this.name = name;
        this.typeDef = typeDef;
        if (charsetName == null) {
            this.charsetName = DEFAULT_CHARSET;
        } else {
            this.charsetName = charsetName;
        }
        this.isKey = isKey;
        this.aggregateType = aggregateType;
        if (isAllowNull == null) {
            this.isAllowNull = true;
            this.isAllowNullImplicit = true;
        } else {
            this.isAllowNull = isAllowNull;
            this.isAllowNullImplicit = false;
        }
        this.defaultValueDef = defaultValueDef;
        this.comment = comment;
    }

    public boolean isAllowNull() {
        return isAllowNull;
    }

    public void setAllowNull(Boolean allowNull) {
        isAllowNull = allowNull;
    }

    // The columns will obey NULL constraint if not specified. The primary key column should abide by the NOT NULL constraint default to be compatible with ANSI.
    // So add a new setPrimaryKeyNonNullable() to set isAllowNull to be true for primary key columns.
    public void setPrimaryKeyNonNullable() {
        if (isAllowNullImplicit) {
            this.isAllowNull = false;
        }
    }

    // only for test
    public String getDefaultValue() {
        if (defaultValueDef.expr instanceof StringLiteral) {
            return ((StringLiteral) defaultValueDef.expr).getValue();
        } else if (defaultValueDef.expr instanceof NullLiteral) {
            return null;
        }
        return null;
    }

    public String getName() {
        return name;
    }

    public AggregateType getAggregateType() {
        return aggregateType;
    }

    public void setAggregateType(AggregateType aggregateType) {
        this.aggregateType = aggregateType;
    }

    public String getCharsetName() {
        return charsetName;
    }

    public boolean isKey() {
        return isKey;
    }

    public void setIsKey(boolean isKey) {
        this.isKey = isKey;
    }

    public TypeDef getTypeDef() {
        return typeDef;
    }

    public Type getType() {
        return typeDef.getType();
    }

    public void analyze(boolean isOlap) throws AnalysisException {
        if (name == null || typeDef == null) {
            throw new AnalysisException("No column name or column type in column definition.");
        }
        FeNameFormat.checkColumnName(name);

        // When string type length is not assigned, it need to be assigned to 1.
        if (typeDef.getType().isScalarType()) {
            final ScalarType targetType = (ScalarType) typeDef.getType();
            if (targetType.getPrimitiveType().isStringType()) {
                if (!targetType.isAssignedStrLenInColDefinition()) {
                    targetType.setLength(1);
                }
            } else {
                // if character setting is not for varchar type, Display unsupported information to the user
                if (!DEFAULT_CHARSET.equalsIgnoreCase(charsetName)) {
                    throw new AnalysisException(
                            "character setting is only supported for type varchar in column definition");
                }
            }
        }

        charsetName = charsetName.toLowerCase();

        if (!charsetNames.contains(charsetName)) {
            throw new AnalysisException("Unknown charset name: " + charsetName);
        }

        // be is not supported yet,so Display unsupported information to the user
        if (!charsetName.equals(DEFAULT_CHARSET)) {
            throw new AnalysisException("charset name " + charsetName + " is not supported yet in column definition");
        }

        if (getAggregateType() == AggregateType.SUM) {
            // For the decimal type we extend to decimal128 to avoid overflow
            typeDef.setType(AggregateType.extendedPrecision(typeDef.getType()));
        }

        typeDef.analyze(null);

        Type type = typeDef.getType();

        if (isKey && isOlap && !type.isKeyType()) {
            if (type.isFloatingPointType()) {
                throw new AnalysisException(
                        String.format("Invalid data type of key column '%s': '%s', use decimal instead", name, type));
            } else {
                throw new AnalysisException(String.format("Invalid data type of key column '%s': '%s'", name, type));
            }
        }

        // A column is a key column if and only if isKey is true.
        // aggregateType == null does not mean that this is a key column,
        // because when creating a UNIQUE KEY table, aggregateType is implicit.
        if (aggregateType != null && aggregateType != AggregateType.NONE) {
            if (isKey) {
                throw new AnalysisException(
                        String.format("Cannot specify aggregate function '%s' for key column '%s'", aggregateType,
                                name));
            }
            if (!aggregateType.checkCompatibility(type)) {
                throw new AnalysisException(
                        String.format("Invalid aggregate function '%s' for '%s'", aggregateType, name));
            }
        } else if (type.isBitmapType() || type.isHllType() || type.isPercentile()) {
            throw new AnalysisException(String.format("No aggregate function specified for '%s'", name));
        }

        if (type.isHllType()) {
            if (defaultValueDef.isSet) {
                throw new AnalysisException(String.format("Invalid default value for '%s'", name));
            }
            defaultValueDef = DefaultValueDef.EMPTY_VALUE;
        }

        if (type.isBitmapType()) {
            if (defaultValueDef.isSet) {
                throw new AnalysisException(String.format("Invalid default value for '%s'", name));
            }
            defaultValueDef = DefaultValueDef.EMPTY_VALUE;
        }

        // If aggregate type is REPLACE_IF_NOT_NULL, we set it nullable.
        // If default value is not set, we set it NULL
        if (aggregateType == AggregateType.REPLACE_IF_NOT_NULL) {
            isAllowNull = true;
            if (!defaultValueDef.isSet) {
                defaultValueDef = DefaultValueDef.NULL_DEFAULT_VALUE;
            }
        }

        if (!isAllowNull && defaultValueDef == DefaultValueDef.NULL_DEFAULT_VALUE) {
            throw new AnalysisException(String.format("Invalid default value for '%s'", name));
        }

        if (defaultValueDef.isSet && defaultValueDef.expr != null) {
            try {
                validateDefaultValue(type, defaultValueDef.expr);
            } catch (AnalysisException e) {
                throw new AnalysisException(String.format("Invalid default value for '%s': %s", name, e.getMessage()));
            }
        }
    }

    public static void validateDefaultValue(Type type, Expr defaultExpr) throws AnalysisException {
        if (defaultExpr instanceof StringLiteral) {
            String defaultValue = ((StringLiteral) defaultExpr).getValue();
            Preconditions.checkNotNull(defaultValue);
            if (type.isComplexType()) {
                throw new AnalysisException(String.format("Default value for complex type '%s' not supported", type));
            }
            ScalarType scalarType = (ScalarType) type;
            // check if default value is valid. if not, some literal constructor will throw AnalysisException
            PrimitiveType primitiveType = scalarType.getPrimitiveType();
            switch (primitiveType) {
                case TINYINT:
                case SMALLINT:
                case INT:
                case BIGINT:
                    IntLiteral intLiteral = new IntLiteral(defaultValue, type);
                    break;
                case LARGEINT:
                    LargeIntLiteral largeIntLiteral = new LargeIntLiteral(defaultValue);
                    break;
                case FLOAT:
                    FloatLiteral floatLiteral = new FloatLiteral(defaultValue);
                    if (floatLiteral.getType().isDouble()) {
                        throw new AnalysisException("Default value will loose precision: " + defaultValue);
                    }
                case DOUBLE:
                    FloatLiteral doubleLiteral = new FloatLiteral(defaultValue);
                    break;
                case DECIMALV2:
                case DECIMAL32:
                case DECIMAL64:
                case DECIMAL128:
                    DecimalLiteral decimalLiteral = new DecimalLiteral(defaultValue);
                    decimalLiteral.checkPrecisionAndScale(scalarType.getScalarPrecision(), scalarType.getScalarScale());
                    break;
                case DATE:
                case DATETIME:
                    DateLiteral dateLiteral = new DateLiteral(defaultValue, type);
                    break;
                case CHAR:
                case VARCHAR:
                case HLL:
                    if (defaultValue.length() > scalarType.getLength()) {
                        throw new AnalysisException("Default value is too long: " + defaultValue);
                    }
                    break;
                case BITMAP:
                    break;
                case BOOLEAN:
                    BoolLiteral boolLiteral = new BoolLiteral(defaultValue);
                    break;
                default:
                    throw new AnalysisException(String.format("Cannot add default value for type '%s'", type));
            }
        } else if (defaultExpr instanceof FunctionCallExpr) {
            FunctionCallExpr functionCallExpr = (FunctionCallExpr) defaultExpr;
            String functionName = functionCallExpr.getFnName().getFunction();
            boolean supported = SUPPORTED_DEFAULT_FNS.contains(functionName + "()");

            if (!supported) {
                throw new AnalysisException(
                        String.format("Default expr for function %s is not supported", functionName));
            }

            // default function current_timestamp currently only support DATETIME type.
            if (FunctionSet.NOW.equalsIgnoreCase(functionName) && type.getPrimitiveType() != PrimitiveType.DATETIME) {
                throw new AnalysisException(String.format("Default function now() for type %s is not supported", type));
            }
            // default function uuid currently only support VARCHAR type.
            if (FunctionSet.UUID.equalsIgnoreCase(functionName) && type.getPrimitiveType() != PrimitiveType.VARCHAR) {
                throw new AnalysisException(String.format("Default function uuid() for type %s is not supported", type));
            }
            if (FunctionSet.UUID.equalsIgnoreCase(functionName) && type.getColumnSize() < 36) {
                throw new AnalysisException("Varchar type length must be greater than 36 for uuid function");
            }
            // default function uuid_numeric currently only support LARGE INT type.
            if (FunctionSet.UUID_NUMERIC.equalsIgnoreCase(functionName) &&
                    type.getPrimitiveType() != PrimitiveType.LARGEINT) {
                throw new AnalysisException(String.format("Default function uuid_numeric() for type %s is not supported",
                        type));
            }
        } else if (defaultExpr instanceof NullLiteral) {
            // nothing to check
        } else {
            throw new AnalysisException(String.format("Unsupported expr %s for default value", defaultExpr));
        }
    }

    public String toSql() {
        StringBuilder sb = new StringBuilder();
        sb.append("`").append(name).append("` ");
        sb.append(typeDef.toSql()).append(" ");

        if (aggregateType != null) {
            sb.append(aggregateType.name()).append(" ");
        }

        if (!isAllowNull) {
            sb.append("NOT NULL ");
        } else {
            // should append NULL to make result can be executed right.
            sb.append("NULL ");
        }

        if (defaultValueDef.isSet) {
            sb.append("DEFAULT ").append(toDefaultExpr(defaultValueDef.expr)).append(" ");
        }
        sb.append("COMMENT \"").append(comment).append("\"");

        return sb.toString();
    }

    public Column toColumn() {
        return new Column(name, typeDef.getType(), isKey, aggregateType, isAllowNull, defaultValueDef, comment);
    }

    private String toDefaultExpr(Expr expr) {
        if (expr instanceof StringLiteral) {
            return "\"" + ((StringLiteral) expr).getValue() + "\"";
        } else if (expr instanceof NullLiteral) {
            return "NULL";
        } else if (expr instanceof FunctionCallExpr) {
            FunctionCallExpr functionCallExpr = (FunctionCallExpr) expr;
            return functionCallExpr.getFnName() + "()";
        }
        return "";
    }

    public boolean defaultValueIsNull() {
        return defaultValueDef.expr instanceof NullLiteral;
    }

    @Override
    public String toString() {
        return toSql();
    }
}
