CREATE TABLE IF NOT EXISTS key_value_table (
  key VARCHAR PRIMARY KEY,
  value VARCHAR,
  updated TIMESTAMPTZ NOT NULL DEFAULT NOW()
)-- Thêm cột status vào bảng (nếu chưa có)
ALTER TABLE key_value_table
ADD COLUMN status VARCHAR DEFAULT 'real';

-- Chèn tài khoản mới với trạng thái "testing"
INSERT INTO key_value_table (key, value, status) VALUES
('125357511', 'Exness', 'market');

-- Cập nhật tài khoản nếu đã tồn tại
UPDATE key_value_table
SET value = 'Exness-MT5', status = 'broker', updated = NOW()
WHERE key = '125357511';

-- Truy vấn các tài khoản đang kiểm tra
SELECT * FROM key_value_table
WHERE status = 'testing';
