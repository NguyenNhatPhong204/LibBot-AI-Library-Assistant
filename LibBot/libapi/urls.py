from django.contrib import admin
from django.urls import include, path

from . import views


urlpatterns = [
    path("admin/", admin.site.urls),
    path("api/", include("books.urls")),
    path("robot-display/", views.robot_display, name="robot_display"),
    path("table/<int:table_id>/", views.table_call_page, name="table_call_page"),
]