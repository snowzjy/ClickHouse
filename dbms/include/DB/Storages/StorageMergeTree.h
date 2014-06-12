#pragma once

#include <DB/Storages/MergeTree/MergeTreeData.h>
#include <DB/Storages/MergeTree/MergeTreeDataSelectExecutor.h>
#include <DB/Storages/MergeTree/MergeTreeDataWriter.h>
#include <DB/Storages/MergeTree/MergeTreeDataMerger.h>
#include <DB/Storages/MergeTree/DiskSpaceMonitor.h>
#include <DB/Storages/MergeTree/BackgroundProcessingPool.h>

namespace DB
{

/** См. описание структуры данных в MergeTreeData.
  */
class StorageMergeTree : public IStorage
{
friend class MergeTreeBlockOutputStream;

public:
	/** Подцепить таблицу с соответствующим именем, по соответствующему пути (с / на конце),
	  *  (корректность имён и путей не проверяется)
	  *  состоящую из указанных столбцов.
	  *
	  * primary_expr_ast	- выражение для сортировки;
	  * date_column_name 	- имя столбца с датой;
	  * index_granularity 	- на сколько строчек пишется одно значение индекса.
	  */
	static StoragePtr create(const String & path_, const String & database_name_, const String & name_,
		NamesAndTypesListPtr columns_,
		const Context & context_,
		ASTPtr & primary_expr_ast_,
		const String & date_column_name_,
		const ASTPtr & sampling_expression_, /// nullptr, если семплирование не поддерживается.
		size_t index_granularity_,
		MergeTreeData::Mode mode_ = MergeTreeData::Ordinary,
		const String & sign_column_ = "",
		const MergeTreeSettings & settings_ = MergeTreeSettings());

	void shutdown();
	~StorageMergeTree();

	std::string getName() const
	{
		return data.getModePrefix() + "MergeTree";
	}

	std::string getTableName() const { return name; }
	bool supportsSampling() const { return data.supportsSampling(); }
	bool supportsFinal() const { return data.supportsFinal(); }
	bool supportsPrewhere() const { return data.supportsPrewhere(); }

	const NamesAndTypesList & getColumnsList() const { return data.getColumnsList(); }

	BlockInputStreams read(
		const Names & column_names,
		ASTPtr query,
		const Settings & settings,
		QueryProcessingStage::Enum & processed_stage,
		size_t max_block_size = DEFAULT_BLOCK_SIZE,
		unsigned threads = 1);

	BlockOutputStreamPtr write(ASTPtr query);

	/** Выполнить очередной шаг объединения кусков.
	  */
	bool optimize()
	{
		return merge(true);
	}

	void drop() override;

	void rename(const String & new_path_to_db, const String & new_name);

	void alter(const ASTAlterQuery::Parameters & params);
	void prepareAlterModify(const ASTAlterQuery::Parameters & params);
	void commitAlterModify(const ASTAlterQuery::Parameters & params);

	bool supportsIndexForIn() const override { return true; }

private:
	String path;
	String name;
	String full_path;
	Increment increment;

	MergeTreeData data;
	MergeTreeDataSelectExecutor reader;
	MergeTreeDataWriter writer;
	MergeTreeDataMerger merger;

	MergeTreeData::DataParts currently_merging;
	Poco::FastMutex currently_merging_mutex;

	Logger * log;

	volatile bool shutdown_called;

	static BackgroundProcessingPool merge_pool;
	BackgroundProcessingPool::TaskHandle merge_task_handle;

	/// Пока существует, помечает части как currently_merging и держит резерв места.
	/// Вероятно, что части будут помечены заранее.
	struct CurrentlyMergingPartsTagger
	{
		MergeTreeData::DataPartsVector parts;
		DiskSpaceMonitor::ReservationPtr reserved_space;
		StorageMergeTree & storage;

		CurrentlyMergingPartsTagger(const MergeTreeData::DataPartsVector & parts_, size_t total_size, StorageMergeTree & storage_)
			: parts(parts_), storage(storage_)
		{
			/// Здесь не лочится мьютекс, так как конструктор вызывается внутри mergeThread, где он уже залочен.
			reserved_space = DiskSpaceMonitor::reserve(storage.full_path, total_size); /// Может бросить исключение.
			for (const auto & part : parts)
			{
				if (storage.currently_merging.count(part))
					throw Exception("Tagging alreagy tagged part " + part->name + ". This is a bug.", ErrorCodes::LOGICAL_ERROR);
			}
			storage.currently_merging.insert(parts.begin(), parts.end());
		}

		~CurrentlyMergingPartsTagger()
		{
			try
			{
				Poco::ScopedLock<Poco::FastMutex> lock(storage.currently_merging_mutex);
				for (const auto & part : parts)
				{
					if (!storage.currently_merging.count(part))
						throw Exception("Untagging already untagged part " + part->name + ". This is a bug.", ErrorCodes::LOGICAL_ERROR);
					storage.currently_merging.erase(part);
				}
			}
			catch (...)
			{
				tryLogCurrentException("~CurrentlyMergingPartsTagger");
			}
		}
	};

	typedef Poco::SharedPtr<CurrentlyMergingPartsTagger> CurrentlyMergingPartsTaggerPtr;

	StorageMergeTree(const String & path_, const String & database_name_, const String & name_,
					NamesAndTypesListPtr columns_,
					const Context & context_,
					ASTPtr & primary_expr_ast_,
					const String & date_column_name_,
					const ASTPtr & sampling_expression_, /// nullptr, если семплирование не поддерживается.
					size_t index_granularity_,
					MergeTreeData::Mode mode_,
					const String & sign_column_,
					const MergeTreeSettings & settings_);

	/** Определяет, какие куски нужно объединять, и объединяет их.
	  * Если aggressive - выбрать куски, не обращая внимание на соотношение размеров и их новизну (для запроса OPTIMIZE).
	  * Возвращает, получилось ли что-нибудь объединить.
	  */
	bool merge(bool aggressive = false, BackgroundProcessingPool::Context * context = nullptr);

	bool mergeTask(BackgroundProcessingPool::Context & context);

	/// Вызывается во время выбора кусков для слияния.
	bool canMergeParts(const MergeTreeData::DataPartPtr & left, const MergeTreeData::DataPartPtr & right);
};

}
