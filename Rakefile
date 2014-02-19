# Copyright (C) 2009-2014 MongoDB Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

require 'fileutils'
require 'json'
require 'pp'
require 'rspec/core/rake_task'
require_relative 'lib/parslet_sql'

# http://www.postgresql.org/docs/9.1/static/sql-createtable.html

def file_to_s(file)
  IO.read(file).chomp
end

FTP_FULLEXPORT_DIR = "ftp.musicbrainz.org/pub/musicbrainz/data/fullexport"
LATEST_FILE = "#{FTP_FULLEXPORT_DIR}/LATEST"
LATEST = file_to_s(LATEST_FILE)
FTP_LATEST_DIR = "#{FTP_FULLEXPORT_DIR}/#{LATEST}"
DATA_LATEST_DIR = "data/fullexport/#{LATEST}"

MONGO_DBPATH = "data/db/#{LATEST}"
MONGOD_PORT = 37017
MONGOD_LOCKPATH = "#{MONGO_DBPATH}/mongod.lock"
MONGOD_LOGPATH = "#{MONGO_DBPATH}/mongod.log"
MONGODB_URI = "mongodb://localhost:#{MONGOD_PORT}"
ENV['MONGODB_URI'] = MONGODB_URI

RSpec::Core::RakeTask.new(:spec)


def path_file_to_s(*args)
  File.join(*(args[0..-2] << file_to_s(File.join(*args))))
end

task :default => [:load_tables] do
  sh "echo Hello World!"
end

file LATEST_FILE do |file|
  sh "wget --recursive ftp://#{file.name}" # need --recursive to retrieve the new version
end

task :fetch => LATEST_FILE do
  sh "wget --recursive -level=1 --continue #{FTP_LATEST_DIR}"
end

task :unarchive => LATEST_FILE do
  mbdump_tar = File.join(FTP_LATEST_DIR, 'mbdump.tar.bz2')
  FileUtils.mkdir_p(DATA_LATEST_DIR)
  Dir.chdir(DATA_LATEST_DIR)
  sh "tar -xf '#{mbdump_tar}'"
end

$CreateTables_sql = "musicbrainz-server/admin/sql/CreateTables.sql"
$CreateTables_sql = 'schema/CreateTables.sql'

file 'schema/create_tables.json' => [ $CreateTables_sql, 'lib/parslet_sql.rb' ] do |file|
  sql_text = IO.read($CreateTables_sql)
  m = CreateTablesParser.new.parse(sql_text)
  File.open(file.name, 'w') {|fio| fio.write(JSON.pretty_generate(m)) }
end

# PK - Primary Key index hint
# references table.column - relation in comment

task :references => 'schema/create_tables.json' do
  JSON.parse(IO.read('schema/create_tables.json')).each do |sql|
    if sql.has_key?('create_table')
      create_table = sql['create_table']
      columns = create_table['columns']
      columns.each do |column|
        comment = column['comment']
        pp comment if comment =~ /references/
      end
    end
  end
end

task :extract => 'schema/create_tables.json' do
  JSON.parse(IO.read('schema/create_tables.json')).each do |sql|
    if sql.has_key?('create_table')
      create_table = sql['create_table']
      table_name = create_table['table_name']
      p table_name
      sh "tar -tf #{MBDUMP} mbdump/#{table_name}"
    end
  end
end

task :load_tables => 'schema/create_tables.json' do
  table_names = Dir["data/fullexport/#{file_to_s(LATEST_FILE)}/mbdump/*"].collect{|file_name| File.basename(file_name) }
  sh "./script/mbdump_to_mongo.rb #{table_names.join(' ')}"
end

task :load_table_test => 'schema/create_tables.json' do
  sh "MONGODB_URI=#{MONGODB_URI} ./script/mbdump_to_mongo.rb artist"
end

namespace :mongo do
  task :start do
    FileUtils.mkdir_p(MONGO_DBPATH) unless File.directory?(MONGO_DBPATH)
    sh "mongod --dbpath #{MONGO_DBPATH} --port #{MONGOD_PORT} --fork --logpath #{MONGOD_LOGPATH}"
  end
  task :status do
    sh "ps -fp #{file_to_s(MONGOD_LOCKPATH)}" if File.size?(MONGOD_LOCKPATH)
  end
  task :stop do
    sh "kill #{file_to_s(MONGOD_LOCKPATH)}" if File.size?(MONGOD_LOCKPATH)
  end
end