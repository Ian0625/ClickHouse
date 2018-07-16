#include <Columns/ColumnWithDictionary.h>
#include <Columns/ColumnUnique.h>
#include <Columns/ColumnFixedString.h>
#include <Columns/ColumnsCommon.h>
#include <Common/typeid_cast.h>
#include <Core/TypeListNumber.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/DataTypeWithDictionary.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime.h>
#include <Parsers/IAST.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int LOGICAL_ERROR;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
}

namespace
{
    const ColumnWithDictionary & getColumnWithDictionary(const IColumn & column)
    {
        return typeid_cast<const ColumnWithDictionary &>(column);
    }

    ColumnWithDictionary & getColumnWithDictionary(IColumn & column)
    {
        return typeid_cast<ColumnWithDictionary &>(column);
    }
}

DataTypeWithDictionary::DataTypeWithDictionary(DataTypePtr dictionary_type_)
        : dictionary_type(std::move(dictionary_type_))
{
    auto inner_type = dictionary_type;
    if (dictionary_type->isNullable())
        inner_type = static_cast<const DataTypeNullable &>(*dictionary_type).getNestedType();

    if (!inner_type->isStringOrFixedString()
        && !inner_type->isDateOrDateTime()
        && !inner_type->isNumber())
        throw Exception("DataTypeWithDictionary is supported only for numbers, strings, Date or DateTime, but got "
                        + dictionary_type->getName(), ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
}

void DataTypeWithDictionary::enumerateStreams(const StreamCallback & callback, SubstreamPath & path) const
{
    path.push_back(Substream::DictionaryKeys);
    dictionary_type->enumerateStreams(callback, path);
    path.back() = Substream::DictionaryIndexes;
    callback(path);
    path.pop_back();
}

struct  KeysSerializationVersion
{
    /// Write keys as full column. No indexes is written. Structure:
    ///   <name>.dict.bin : [version - 32 bits][keys]
    ///   <name>.dict.mrk : [marks for keys]
    // FullColumn = 0,
    /// Write all keys in serializePostfix and read in deserializePrefix.
    ///   <name>.dict.bin : [version - 32 bits][indexes type - 32 bits][keys]
    ///   <name>.bin : [indexes]
    ///   <name>.mrk : [marks for indexes]
    // SingleDictionary,
    /// Write distinct set of keys for each granule. Structure:
    ///   <name>.dict.bin : [version - 32 bits][indexes type - 32 bits][keys]
    ///   <name>.dict.mrk : [marks for keys]
    ///   <name>.bin : [indexes]
    ///   <name>.mrk : [marks for indexes]
    // DictionaryPerGranule,

    enum Value
    {
        SingleDictionaryWithAdditionalKeysPerBlock = 1,
    };

    Value value;

    static void checkVersion(UInt64 version)
    {
        if (version != SingleDictionaryWithAdditionalKeysPerBlock)
            throw Exception("Invalid version for DataTypeWithDictionary key column.", ErrorCodes::LOGICAL_ERROR);
    }

    KeysSerializationVersion(UInt64 version) : value(static_cast<Value>(version)) { checkVersion(version); }
};

struct IndexesSerializationType
{
    using SerializationType = UInt64;
    static constexpr UInt64 NeedGlobalDictionaryBit = 1u << 8u;
    static constexpr UInt64 HasAdditionalKeysBit = 1u << 9u;

    enum Type
    {
        TUInt8 = 0,
        TUInt16,
        TUInt32,
        TUInt64,
    };

    Type type;
    bool has_additional_keys;
    bool need_global_dictionary;

    static constexpr SerializationType resetFlags(SerializationType type)
    {
        return type & (~(HasAdditionalKeysBit | NeedGlobalDictionaryBit));
    }

    static void checkType(SerializationType type)
    {
        UInt64 value = resetFlags(type);
        if (value <= TUInt64)
            return;

        throw Exception("Invalid type for DataTypeWithDictionary index column.", ErrorCodes::LOGICAL_ERROR);
    }

    void serialize(WriteBuffer & buffer) const
    {
        SerializationType val = type;
        if (has_additional_keys)
            val |= HasAdditionalKeysBit;
        if (need_global_dictionary)
            val |= NeedGlobalDictionaryBit;
        writeIntBinary(val, buffer);
    }

    void deserialize(ReadBuffer & buffer)
    {
        SerializationType val;
        readIntBinary(val, buffer);
        checkType(val);
        has_additional_keys = (val & HasAdditionalKeysBit) != 0;
        need_global_dictionary = (val & NeedGlobalDictionaryBit) != 0;
        type = static_cast<Type>(resetFlags(val));
    }

    IndexesSerializationType(const IColumn & column, bool has_additional_keys, bool need_global_dictionary)
        : has_additional_keys(has_additional_keys), need_global_dictionary(need_global_dictionary)
    {
        if (typeid_cast<const ColumnUInt8 *>(&column))
            type = TUInt8;
        else if (typeid_cast<const ColumnUInt16 *>(&column))
            type = TUInt16;
        else if (typeid_cast<const ColumnUInt32 *>(&column))
            type = TUInt32;
        else if (typeid_cast<const ColumnUInt64 *>(&column))
            type = TUInt64;
        else
            throw Exception("Invalid Indexes column for IndexesSerializationType. Expected ColumnUInt*, got "
                            + column.getName(), ErrorCodes::LOGICAL_ERROR);
    }

    DataTypePtr getDataType() const
    {
        if (type == TUInt8)
            return std::make_shared<DataTypeUInt8>();
        if (type == TUInt16)
            return std::make_shared<DataTypeUInt16>();
        if (type == TUInt32)
            return std::make_shared<DataTypeUInt32>();
        if (type == TUInt64)
            return std::make_shared<DataTypeUInt64>();

        throw Exception("Can't create DataType from IndexesSerializationType.", ErrorCodes::LOGICAL_ERROR);
    }

    IndexesSerializationType() = default;
};

struct SerializeStateWithDictionary : public IDataType::SerializeBinaryBulkState
{
    KeysSerializationVersion key_version;
    MutableColumnUniquePtr global_dictionary;

    explicit SerializeStateWithDictionary(
        UInt64 key_version,
        MutableColumnUniquePtr && column_unique)
        : key_version(key_version)
        , global_dictionary(std::move(column_unique)) {}
};

struct DeserializeStateWithDictionary : public IDataType::DeserializeBinaryBulkState
{
    KeysSerializationVersion key_version;
    ColumnUniquePtr global_dictionary;

    IndexesSerializationType index_type;
    MutableColumnPtr additional_keys;
    UInt64 num_pending_rows = 0;

    explicit DeserializeStateWithDictionary(UInt64 key_version) : key_version(key_version) {}
};

static SerializeStateWithDictionary * checkAndGetWithDictionarySerializeState(
    IDataType::SerializeBinaryBulkStatePtr & state)
{
    if (!state)
        throw Exception("Got empty state for DataTypeWithDictionary.", ErrorCodes::LOGICAL_ERROR);

    auto * with_dictionary_state = typeid_cast<SerializeStateWithDictionary *>(state.get());
    if (!with_dictionary_state)
        throw Exception("Invalid SerializeBinaryBulkState for DataTypeWithDictionary. Expected: "
                        + demangle(typeid(SerializeStateWithDictionary).name()) + ", got "
                        + demangle(typeid(*state).name()), ErrorCodes::LOGICAL_ERROR);

    return with_dictionary_state;
}

static DeserializeStateWithDictionary * checkAndGetWithDictionaryDeserializeState(
    IDataType::DeserializeBinaryBulkStatePtr & state)
{
    if (!state)
        throw Exception("Got empty state for DataTypeWithDictionary.", ErrorCodes::LOGICAL_ERROR);

    auto * with_dictionary_state = typeid_cast<DeserializeStateWithDictionary *>(state.get());
    if (!with_dictionary_state)
        throw Exception("Invalid DeserializeBinaryBulkState for DataTypeWithDictionary. Expected: "
                        + demangle(typeid(DeserializeStateWithDictionary).name()) + ", got "
                        + demangle(typeid(*state).name()), ErrorCodes::LOGICAL_ERROR);

    return with_dictionary_state;
}

void DataTypeWithDictionary::serializeBinaryBulkStatePrefix(
    SerializeBinaryBulkSettings & settings,
    SerializeBinaryBulkStatePtr & state) const
{
    settings.path.push_back(Substream::DictionaryKeys);
    auto * stream = settings.getter(settings.path);
    settings.path.pop_back();

    if (!stream)
        throw Exception("Got empty stream in DataTypeWithDictionary::serializeBinaryBulkStatePrefix",
                        ErrorCodes::LOGICAL_ERROR);

    /// Write version and create SerializeBinaryBulkState.
    UInt64 key_version = KeysSerializationVersion::SingleDictionaryWithAdditionalKeysPerBlock;

    writeIntBinary(key_version, *stream);

    auto column_unique = createColumnUnique(*dictionary_type);
    state = std::make_shared<SerializeStateWithDictionary>(key_version, std::move(column_unique));
}

void DataTypeWithDictionary::serializeBinaryBulkStateSuffix(
    SerializeBinaryBulkSettings & settings,
    SerializeBinaryBulkStatePtr & state) const
{
    auto * state_with_dictionary = checkAndGetWithDictionarySerializeState(state);
    KeysSerializationVersion::checkVersion(state_with_dictionary->key_version.value);

    if (state_with_dictionary->global_dictionary && settings.max_dictionary_size)
    {
        auto nested_column = state_with_dictionary->global_dictionary->getNestedNotNullableColumn();

        settings.path.push_back(Substream::DictionaryKeys);
        auto * stream = settings.getter(settings.path);
        settings.path.pop_back();

        if (!stream)
            throw Exception("Got empty stream in DataTypeWithDictionary::serializeBinaryBulkStateSuffix",
                            ErrorCodes::LOGICAL_ERROR);

        UInt64 num_keys = nested_column->size();
        writeIntBinary(num_keys, *stream);
        removeNullable(dictionary_type)->serializeBinaryBulk(*nested_column, *stream, 0, num_keys);
    }
}

void DataTypeWithDictionary::deserializeBinaryBulkStatePrefix(
    DeserializeBinaryBulkSettings & settings,
    DeserializeBinaryBulkStatePtr & state) const
{
    settings.path.push_back(Substream::DictionaryKeys);
    auto * stream = settings.getter(settings.path);
    settings.path.pop_back();

    if (!stream)
        throw Exception("Got empty stream in DataTypeWithDictionary::deserializeBinaryBulkStatePrefix",
                        ErrorCodes::LOGICAL_ERROR);

    UInt64 keys_version;
    readIntBinary(keys_version, *stream);

    state = std::make_shared<DeserializeStateWithDictionary>(keys_version);
}

namespace
{
    template <typename T>
    PaddedPODArray<T> * getIndexesData(IColumn & indexes)
    {
        auto * column = typeid_cast<ColumnVector<T> *>(&indexes);
        if (column)
            return &column->getData();

        return nullptr;
    }

    template <typename T>
    MutableColumnPtr mapIndexWithOverflow(PaddedPODArray<T> & index, size_t max_val)
    {
        HashMap<T, T> hash_map;

        for (auto val : index)
        {
            if (val < max_val)
                hash_map.insert({val, hash_map.size()});
        }

        auto index_map_col = ColumnVector<T>::create();
        auto & index_data = index_map_col->getData();

        index_data.resize(hash_map.size());
        for (auto val : hash_map)
            index_data[val.second] = val.first;

        for (auto & val : index)
            val = val < max_val ? hash_map[val]
                                : val - max_val + hash_map.size();

        return index_map_col;
    }

    /// Update column and return map with old indexes.
    /// Let N is the number of distinct values which are less than max_size;
    ///     old_column - column before function call;
    ///     new_column - column after function call;
    ///     map - function result (map.size() is N):
    /// * if old_column[i] < max_size, than
    ///       map[new_column[i]] = old_column[i]
    /// * else
    ///       new_column[i] = old_column[i] - max_size + N
    MutableColumnPtr mapIndexWithOverflow(IColumn & column, size_t max_size)
    {
        if (auto * data_uint8 = getIndexesData<UInt8>(column))
            return mapIndexWithOverflow(*data_uint8, max_size);
        else if (auto * data_uint16 = getIndexesData<UInt16>(column))
            return mapIndexWithOverflow(*data_uint16, max_size);
        else if (auto * data_uint32 = getIndexesData<UInt32>(column))
            return mapIndexWithOverflow(*data_uint32, max_size);
        else if (auto * data_uint64 = getIndexesData<UInt64>(column))
            return mapIndexWithOverflow(*data_uint64, max_size);
        else
            throw Exception("Indexes column for makeIndexWithOverflow must be ColumnUInt, got" + column.getName(),
                            ErrorCodes::LOGICAL_ERROR);
    }
}

void DataTypeWithDictionary::serializeBinaryBulkWithMultipleStreams(
    const IColumn & column,
    size_t offset,
    size_t limit,
    SerializeBinaryBulkSettings & settings,
    SerializeBinaryBulkStatePtr & state) const
{
    settings.path.push_back(Substream::DictionaryKeys);
    auto * keys_stream = settings.getter(settings.path);
    settings.path.back() = Substream::DictionaryIndexes;
    auto * indexes_stream = settings.getter(settings.path);
    settings.path.pop_back();

    if (!keys_stream && !indexes_stream)
        return;

    if (!keys_stream)
        throw Exception("Got empty stream for DataTypeWithDictionary keys.", ErrorCodes::LOGICAL_ERROR);

    if (!indexes_stream)
        throw Exception("Got empty stream for DataTypeWithDictionary indexes.", ErrorCodes::LOGICAL_ERROR);

    const ColumnWithDictionary & column_with_dictionary = typeid_cast<const ColumnWithDictionary &>(column);

    auto * state_with_dictionary = checkAndGetWithDictionarySerializeState(state);
    auto & global_dictionary = state_with_dictionary->global_dictionary;
    KeysSerializationVersion::checkVersion(state_with_dictionary->key_version.value);

    size_t max_limit = column.size() - offset;
    limit = limit ? std::min(limit, max_limit) : max_limit;

    auto sub_column = column_with_dictionary.cutAndCompact(offset, limit);
    ColumnPtr positions = sub_column->getIndexesPtr();
    ColumnPtr keys = sub_column->getDictionary().getNestedColumn();

    if (settings.max_dictionary_size)
    {
        /// Insert used_keys into global dictionary and update sub_index.
        auto indexes_with_overflow = global_dictionary->uniqueInsertRangeWithOverflow(*keys, 0, keys->size(),
                                                                                      settings.max_dictionary_size);
        positions = indexes_with_overflow.indexes->index(*positions, 0);
        keys = std::move(indexes_with_overflow.overflowed_keys);
    }

    if (auto nullable_keys = typeid_cast<const ColumnNullable *>(keys.get()))
        keys = nullable_keys->getNestedColumnPtr();

    bool need_additional_keys = !keys->empty();
    bool need_dictionary = settings.max_dictionary_size != 0;
    bool need_write_dictionary = settings.use_new_dictionary_on_overflow
                                 && global_dictionary->size() >= settings.max_dictionary_size;

    IndexesSerializationType index_version(*positions, need_additional_keys, need_dictionary);
    index_version.serialize(*indexes_stream);

    if (need_write_dictionary)
    {
        const auto & nested_column = global_dictionary->getNestedNotNullableColumn();
        UInt64 num_keys = nested_column->size();
        writeIntBinary(num_keys, *keys_stream);
        removeNullable(dictionary_type)->serializeBinaryBulk(*nested_column, *keys_stream, 0, num_keys);
        state_with_dictionary->global_dictionary = createColumnUnique(*dictionary_type);
    }

    if (need_additional_keys)
    {
        UInt64 num_keys = keys->size();
        writeIntBinary(num_keys, *indexes_stream);
        removeNullable(dictionary_type)->serializeBinaryBulk(*keys, *indexes_stream, 0, num_keys);
    }

    UInt64 num_rows = positions->size();
    writeIntBinary(num_rows, *indexes_stream);
    index_version.getDataType()->serializeBinaryBulk(*positions, *indexes_stream, 0, num_rows);
}

void DataTypeWithDictionary::deserializeBinaryBulkWithMultipleStreams(
    IColumn & column,
    size_t limit,
    DeserializeBinaryBulkSettings & settings,
    DeserializeBinaryBulkStatePtr & state) const
{
    ColumnWithDictionary & column_with_dictionary = typeid_cast<ColumnWithDictionary &>(column);

    auto * state_with_dictionary = checkAndGetWithDictionaryDeserializeState(state);
    KeysSerializationVersion::checkVersion(state_with_dictionary->key_version.value);

    settings.path.push_back(Substream::DictionaryKeys);
    auto * keys_stream = settings.getter(settings.path);
    settings.path.back() = Substream::DictionaryIndexes;
    auto * indexes_stream = settings.getter(settings.path);
    settings.path.pop_back();

    if (!keys_stream && !indexes_stream)
        return;

    if (!keys_stream)
        throw Exception("Got empty stream for DataTypeWithDictionary keys.", ErrorCodes::LOGICAL_ERROR);

    if (!indexes_stream)
        throw Exception("Got empty stream for DataTypeWithDictionary indexes.", ErrorCodes::LOGICAL_ERROR);

    auto readDictionary = [this, state_with_dictionary, keys_stream, &column_with_dictionary]()
    {
        UInt64 num_keys;
        readIntBinary(num_keys, *keys_stream);

        auto keys_type = removeNullable(dictionary_type);
        auto global_dict_keys = keys_type->createColumn();
        keys_type->deserializeBinaryBulk(*global_dict_keys, *keys_stream, num_keys, 0);

        auto column_unique = createColumnUnique(*dictionary_type, std::move(global_dict_keys));
        state_with_dictionary->global_dictionary = std::move(column_unique);
    };

    auto readAdditionalKeys = [this, state_with_dictionary, indexes_stream]()
    {
        UInt64 num_keys;
        readIntBinary(num_keys, *indexes_stream);
        auto keys_type = removeNullable(dictionary_type);
        state_with_dictionary->additional_keys = keys_type->createColumn();
        keys_type->deserializeBinaryBulk(*state_with_dictionary->additional_keys, *indexes_stream, num_keys, 0);
    };

    auto readIndexes = [this, state_with_dictionary, indexes_stream, &column_with_dictionary](UInt64 num_rows)
    {
        auto indexes_type = state_with_dictionary->index_type.getDataType();
        MutableColumnPtr indexes_column = indexes_type->createColumn();
        indexes_type->deserializeBinaryBulk(*indexes_column, *indexes_stream, num_rows, 0);

        auto & global_dictionary = state_with_dictionary->global_dictionary;
        const auto & additional_keys = state_with_dictionary->additional_keys;

        bool has_additional_keys = state_with_dictionary->index_type.has_additional_keys;
        bool column_is_empty = column_with_dictionary.empty();
        bool column_with_global_dictionary = &column_with_dictionary.getDictionary() == global_dictionary.get();

        if (!has_additional_keys && (column_is_empty || column_with_global_dictionary))
        {
            if (column_is_empty)
                column_with_dictionary.setSharedDictionary(global_dictionary);

            auto local_column = ColumnWithDictionary::create(global_dictionary, std::move(indexes_column));
            column_with_dictionary.insertRangeFrom(*local_column, 0, num_rows);
        }
        else if (!state_with_dictionary->index_type.need_global_dictionary)
        {
            column_with_dictionary.insertRangeFromDictionaryEncodedColumn(*additional_keys, *indexes_column);
        }
        else
        {
            auto index_map = mapIndexWithOverflow(*indexes_column, global_dictionary->size());
            auto keys = (*std::move(global_dictionary->getNestedColumn()->index(*index_map, 0))).mutate();

            if (additional_keys)
                keys->insertRangeFrom(*additional_keys, 0, additional_keys->size());

            column_with_dictionary.insertRangeFromDictionaryEncodedColumn(*keys, *indexes_column);
        }
    };

    while (limit)
    {
        if (state_with_dictionary->num_pending_rows == 0)
        {
            if (indexes_stream->eof())
                break;

            state_with_dictionary->index_type.deserialize(*indexes_stream);

            if (state_with_dictionary->index_type.need_global_dictionary && !state_with_dictionary->global_dictionary)
                readDictionary();

            if (state_with_dictionary->index_type.has_additional_keys)
                readAdditionalKeys();
            else
                state_with_dictionary->additional_keys = nullptr;

            readIntBinary(state_with_dictionary->num_pending_rows, *indexes_stream);
        }

        size_t num_rows_to_read = std::min(limit, state_with_dictionary->num_pending_rows);
        readIndexes(num_rows_to_read);
        limit -= num_rows_to_read;
        state_with_dictionary->num_pending_rows -= num_rows_to_read;
    }
}

void DataTypeWithDictionary::serializeBinary(const Field & field, WriteBuffer & ostr) const
{
    dictionary_type->serializeBinary(field, ostr);
}
void DataTypeWithDictionary::deserializeBinary(Field & field, ReadBuffer & istr) const
{
    dictionary_type->deserializeBinary(field, istr);
}

template <typename ... Args>
void DataTypeWithDictionary::serializeImpl(
        const IColumn & column, size_t row_num, WriteBuffer & ostr,
        DataTypeWithDictionary::SerealizeFunctionPtr<Args ...> func, Args & ... args) const
{
    auto & column_with_dictionary = getColumnWithDictionary(column);
    size_t unique_row_number = column_with_dictionary.getIndexes().getUInt(row_num);
    (dictionary_type.get()->*func)(*column_with_dictionary.getDictionary().getNestedColumn(), unique_row_number, ostr, std::forward<Args>(args)...);
}

template <typename ... Args>
void DataTypeWithDictionary::deserializeImpl(
        IColumn & column, ReadBuffer & istr,
        DataTypeWithDictionary::DeserealizeFunctionPtr<Args ...> func, Args & ... args) const
{
    auto & column_with_dictionary = getColumnWithDictionary(column);
    auto temp_column = column_with_dictionary.getDictionary().cloneEmpty();

    (dictionary_type.get()->*func)(*temp_column, istr, std::forward<Args>(args)...);

    column_with_dictionary.insertFromFullColumn(*temp_column, 0);
}

namespace
{
    template <typename Creator>
    struct CreateColumnVector
    {
        MutableColumnUniquePtr & column;
        const IDataType & keys_type;
        const Creator & creator;

        CreateColumnVector(MutableColumnUniquePtr & column, const IDataType & keys_type, const Creator & creator)
                : column(column), keys_type(keys_type), creator(creator)
        {
        }

        template <typename T, size_t>
        void operator()()
        {
            if (typeid_cast<const DataTypeNumber<T> *>(&keys_type))
                column = creator((ColumnVector<T> *)(nullptr));
        }
    };
}

template <typename Creator>
MutableColumnUniquePtr DataTypeWithDictionary::createColumnUniqueImpl(const IDataType & keys_type,
                                                                      const Creator & creator)
{
    auto * type = &keys_type;
    if (auto * nullable_type = typeid_cast<const DataTypeNullable *>(&keys_type))
        type = nullable_type->getNestedType().get();

    if (type->isString())
        return creator((ColumnString *)(nullptr));
    if (type->isFixedString())
        return creator((ColumnFixedString *)(nullptr));
    if (typeid_cast<const DataTypeDate *>(type))
        return creator((ColumnVector<UInt16> *)(nullptr));
    if (typeid_cast<const DataTypeDateTime *>(type))
        return creator((ColumnVector<UInt32> *)(nullptr));
    if (type->isNumber())
    {
        MutableColumnUniquePtr column;
        TypeListNumbers::forEach(CreateColumnVector(column, *type, creator));

        if (!column)
            throw Exception("Unexpected numeric type: " + type->getName(), ErrorCodes::LOGICAL_ERROR);

        return column;
    }

    throw Exception("Unexpected dictionary type for DataTypeWithDictionary: " + type->getName(),
                    ErrorCodes::LOGICAL_ERROR);
}


MutableColumnUniquePtr DataTypeWithDictionary::createColumnUnique(const IDataType & keys_type)
{
    auto creator = [&](auto x)
    {
        using ColumnType = typename std::remove_pointer<decltype(x)>::type;
        return ColumnUnique<ColumnType>::create(keys_type);
    };
    return createColumnUniqueImpl(keys_type, creator);
}

MutableColumnUniquePtr DataTypeWithDictionary::createColumnUnique(const IDataType & keys_type, MutableColumnPtr && keys)
{
    auto creator = [&](auto x)
    {
        using ColumnType = typename std::remove_pointer<decltype(x)>::type;
        return ColumnUnique<ColumnType>::create(std::move(keys), keys_type.isNullable());
    };
    return createColumnUniqueImpl(keys_type, creator);
}

MutableColumnPtr DataTypeWithDictionary::createColumn() const
{
    MutableColumnPtr indexes = DataTypeUInt8().createColumn();
    MutableColumnPtr dictionary = createColumnUnique(*dictionary_type);
    return ColumnWithDictionary::create(std::move(dictionary), std::move(indexes));
}

bool DataTypeWithDictionary::equals(const IDataType & rhs) const
{
    if (typeid(rhs) != typeid(*this))
        return false;

    auto & rhs_with_dictionary = static_cast<const DataTypeWithDictionary &>(rhs);
    return dictionary_type->equals(*rhs_with_dictionary.dictionary_type);
}


static DataTypePtr create(const ASTPtr & arguments)
{
    if (!arguments || arguments->children.size() != 1)
        throw Exception("WithDictionary data type family must have single argument - type of elements",
                        ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

    return std::make_shared<DataTypeWithDictionary>(DataTypeFactory::instance().get(arguments->children[0]));
}

void registerDataTypeWithDictionary(DataTypeFactory & factory)
{
    factory.registerDataType("WithDictionary", create);
}

}
