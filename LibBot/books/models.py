from django.db import models
from django.core.validators import MinValueValidator

class Location(models.Model):
    # === Vị trí kệ/hàng trong thư viện ===
    shelf = models.CharField(max_length=20)   # ví dụ: "Kệ A1"
    row = models.CharField(max_length=10)     # ví dụ: "Hàng 3"
    floor = models.IntegerField(default=1)    # tầng
    map_image = models.ImageField(upload_to='maps/', blank=True, null=True)  # sơ đồ khu vực

    def __str__(self):
        return f"{self.shelf} - Hàng {self.row} (Tầng {self.floor})"

class Book(models.Model):
    title = models.CharField(max_length=255)
    author = models.CharField(max_length=100)
    isbn = models.CharField(max_length=13, unique=True, blank=True, null=True)
    category = models.CharField(max_length=50, blank=True)
    quantity = models.PositiveIntegerField(default=0, validators=[MinValueValidator(0)])
    location = models.ForeignKey(Location, on_delete=models.SET_NULL, null=True, blank=True)
    # === Thêm trạng thái chung ===
    is_available = models.BooleanField(default=True)

    def __str__(self):
        return self.title

    def save(self, *args, **kwargs):
        # Tự động cập nhật is_available dựa trên quantity
        self.is_available = (self.quantity > 0)
        super().save(*args, **kwargs)

class BorrowRecord(models.Model):
    # === Ghi nhận mượn sách (để biết sách nào đang được mượn, ai mượn) ===
    book = models.ForeignKey(Book, on_delete=models.CASCADE)
    borrower_name = models.CharField(max_length=100)  # tạm thời chưa có model Student
    borrow_date = models.DateTimeField(auto_now_add=True)
    due_date = models.DateField()
    return_date = models.DateTimeField(null=True, blank=True)

    def __str__(self):
        return f"{self.book.title} - {self.borrower_name} - {'Đã trả' if self.return_date else 'Chưa trả'}"

class RobotTaskLog(models.Model):
    STATUS_CHOICES = [
        ("created", "Created"),
        ("moving", "Moving"),
        ("arrived", "Arrived"),
        ("returning", "Returning"),
        ("completed", "Completed"),
        ("stopped", "Stopped"),
        ("timeout", "Timeout"),
        ("error", "Error"),
    ]

    table_id = models.IntegerField(null=True, blank=True)
    command = models.CharField(max_length=50, blank=True)
    status = models.CharField(max_length=20, choices=STATUS_CHOICES, default="created")

    esp_response = models.TextField(blank=True)
    error_message = models.TextField(blank=True)

    started_at = models.DateTimeField(auto_now_add=True)
    arrived_at = models.DateTimeField(null=True, blank=True)
    finished_at = models.DateTimeField(null=True, blank=True)

    last_update = models.DateTimeField(auto_now=True)

    def __str__(self):
        return f"RobotTaskLog(table={self.table_id}, command={self.command}, status={self.status})"